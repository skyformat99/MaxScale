/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file monitor.c  - The monitor module management routines
 */
#include <maxscale/monitor.hh>

#include <atomic>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <string>
#include <sstream>
#include <set>
#include <zlib.h>
#include <sys/stat.h>
#include <vector>
#include <mutex>

#include <maxscale/alloc.h>
#include <maxscale/clock.h>
#include <maxscale/json_api.hh>
#include <maxscale/mariadb.hh>
#include <maxscale/mysql_utils.hh>
#include <maxscale/paths.h>
#include <maxscale/pcre2.h>
#include <maxscale/routingworker.hh>
#include <maxscale/secrets.h>
#include <maxscale/utils.hh>
#include <maxscale/json_api.hh>
#include <mysqld_error.h>

#include "internal/config.hh"
#include "internal/externcmd.hh"
#include "internal/monitor.hh"
#include "internal/modules.hh"
#include "internal/server.hh"
#include "internal/service.hh"

/** Schema version, journals must have a matching version */
#define MMB_SCHEMA_VERSION 2

/** Constants for byte lengths of the values */
#define MMB_LEN_BYTES          4
#define MMB_LEN_SCHEMA_VERSION 1
#define MMB_LEN_CRC32          4
#define MMB_LEN_VALUE_TYPE     1
#define MMB_LEN_SERVER_STATUS  8

/** Type of the stored value */
enum stored_value_type
{
    SVT_SERVER = 1,     // Generic server state information
    SVT_MASTER = 2,     // The master server name
};

using std::string;
using std::set;
using Guard = std::lock_guard<std::mutex>;
using maxscale::Monitor;
using maxscale::MonitorServer;

const char CN_BACKEND_CONNECT_ATTEMPTS[] = "backend_connect_attempts";
const char CN_BACKEND_CONNECT_TIMEOUT[] = "backend_connect_timeout";
const char CN_BACKEND_READ_TIMEOUT[] = "backend_read_timeout";
const char CN_BACKEND_WRITE_TIMEOUT[] = "backend_write_timeout";
const char CN_DISK_SPACE_CHECK_INTERVAL[] = "disk_space_check_interval";
const char CN_EVENTS[] = "events";
const char CN_JOURNAL_MAX_AGE[] = "journal_max_age";
const char CN_MONITOR_INTERVAL[] = "monitor_interval";
const char CN_SCRIPT[] = "script";
const char CN_SCRIPT_TIMEOUT[] = "script_timeout";

namespace
{

class ThisUnit
{
public:

    /**
     * Mark a monitor as the monitor of the server. A server may only be monitored by one monitor.
     *
     * @param server Server to claim. The name is not checked to be a valid server name.
     * @param new_owner Monitor which claims the server
     * @param existing_owner If server is already monitored, the owning monitor name is written here
     * @return True if success, false if server was claimed by another monitor
     */
    bool claim_server(const string& server, const string& new_owner, string* existing_owner)
    {
        mxb_assert(Monitor::is_admin_thread());
        bool claim_success = false;
        auto iter = m_server_owners.find(server);
        if (iter != m_server_owners.end())
        {
            // Server is already claimed by a monitor.
            *existing_owner = iter->second;
        }
        else
        {
            m_server_owners[server] = new_owner;
            claim_success = true;
        }
        return claim_success;
    }

    /**
     * Mark a server as unmonitored.
     *
     * @param server The server name
     */
    void release_server(const string& server)
    {
        mxb_assert(Monitor::is_admin_thread());
        auto iter = m_server_owners.find(server);
        mxb_assert(iter != m_server_owners.end());
        m_server_owners.erase(iter);
    }


    string claimed_by(const string& server)
    {
        mxb_assert(Monitor::is_admin_thread());
        string rval;
        auto iter = m_server_owners.find(server);
        if (iter != m_server_owners.end())
        {
            rval = iter->second;
        }
        return rval;
    }

private:
    // Global map of servername->monitorname. Not mutexed, as this should only be accessed
    // from the admin thread.
    std::map<string, string> m_server_owners;
};

ThisUnit this_unit;

const char* monitor_state_to_string(monitor_state_t state)
{
    switch (state)
    {
    case MONITOR_STATE_RUNNING:
        return "Running";

    case MONITOR_STATE_STOPPED:
        return "Stopped";

    default:
        mxb_assert(false);
        return "Unknown";
    }
}

/** Server type specific bits */
const uint64_t server_type_bits = SERVER_MASTER | SERVER_SLAVE | SERVER_JOINED;

/** All server bits */
const uint64_t all_server_bits = SERVER_RUNNING | SERVER_MAINT | SERVER_MASTER | SERVER_SLAVE
    | SERVER_JOINED;

const char journal_name[] = "monitor.dat";
const char journal_template[] = "%s/%s/%s";

/**
 * @brief Remove .tmp suffix and rename file
 *
 * @param src File to rename
 * @return True if file was successfully renamed
 */
bool rename_tmp_file(Monitor* monitor, const char* src)
{
    bool rval = true;
    char dest[PATH_MAX + 1];
    snprintf(dest, sizeof(dest), journal_template, get_datadir(), monitor->name(), journal_name);

    if (rename(src, dest) == -1)
    {
        rval = false;
        MXS_ERROR("Failed to rename journal file '%s' to '%s': %d, %s",
                  src,
                  dest,
                  errno,
                  mxs_strerror(errno));
    }

    return rval;
}

/**
 * @brief Open temporary file
 *
 * @param monitor Monitor
 * @param path Output where the path is stored
 * @return Opened file or NULL on error
 */
FILE* open_tmp_file(Monitor* monitor, char* path)
{
    int nbytes = snprintf(path, PATH_MAX, journal_template, get_datadir(), monitor->name(), "");
    int max_bytes = PATH_MAX - (int)sizeof(journal_name);
    FILE* rval = NULL;

    if (nbytes < max_bytes && mxs_mkdir_all(path, 0744))
    {
        strcat(path, journal_name);
        strcat(path, "XXXXXX");
        int fd = mkstemp(path);

        if (fd == -1)
        {
            MXS_ERROR("Failed to open file '%s': %d, %s", path, errno, mxs_strerror(errno));
        }
        else
        {
            rval = fdopen(fd, "w");
        }
    }
    else
    {
        MXS_ERROR("Path is too long: %d characters exceeds the maximum path "
                  "length of %d bytes",
                  nbytes,
                  max_bytes);
    }

    return rval;
}

/**
 * @brief Store server data to in-memory buffer
 *
 * @param monitor Monitor
 * @param data Pointer to in-memory buffer used for storage, should be at least
 *             PATH_MAX bytes long
 * @param size Size of @c data
 */
void store_data(Monitor* monitor, MonitorServer* master, uint8_t* data, uint32_t size)
{
    uint8_t* ptr = data;

    /** Store the data length */
    mxb_assert(sizeof(size) == MMB_LEN_BYTES);
    ptr = mxs_set_byte4(ptr, size);

    /** Then the schema version */
    *ptr++ = MMB_SCHEMA_VERSION;

    /** Store the states of all servers */
    for (MonitorServer* db : monitor->m_servers)
    {
        *ptr++ = (char)SVT_SERVER;                                  // Value type
        memcpy(ptr, db->server->name(), strlen(db->server->name()));// Name of the server
        ptr += strlen(db->server->name());
        *ptr++ = '\0';      // Null-terminate the string

        auto status = db->server->status;
        static_assert(sizeof(status) == MMB_LEN_SERVER_STATUS,
                      "Status size should be MMB_LEN_SERVER_STATUS bytes");
        ptr = maxscale::set_byteN(ptr, status, MMB_LEN_SERVER_STATUS);
    }

    /** Store the current root master if we have one */
    if (master)
    {
        *ptr++ = (char)SVT_MASTER;
        memcpy(ptr, master->server->name(), strlen(master->server->name()));
        ptr += strlen(master->server->name());
        *ptr++ = '\0';      // Null-terminate the string
    }

    /** Calculate the CRC32 for the complete payload minus the CRC32 bytes */
    uint32_t crc = crc32(0L, NULL, 0);
    crc = crc32(crc, (uint8_t*)data + MMB_LEN_BYTES, size - MMB_LEN_CRC32);
    mxb_assert(sizeof(crc) == MMB_LEN_CRC32);

    ptr = mxs_set_byte4(ptr, crc);
    mxb_assert(ptr - data == size + MMB_LEN_BYTES);
}

/**
 * Check that memory area contains a null terminator
 */
static bool has_null_terminator(const char* data, const char* end)
{
    while (data < end)
    {
        if (*data == '\0')
        {
            return true;
        }
        data++;
    }

    return false;
}

/**
 * Process a generic server
 */
const char* process_server(Monitor* monitor, const char* data, const char* end)
{
    for (MonitorServer* db : monitor->m_servers)
    {
        if (strcmp(db->server->name(), data) == 0)
        {
            const unsigned char* sptr = (unsigned char*)strchr(data, '\0');
            mxb_assert(sptr);
            sptr++;

            uint64_t status = maxscale::get_byteN(sptr, MMB_LEN_SERVER_STATUS);
            db->mon_prev_status = status;
            db->server->set_status(status);
            db->set_pending_status(status);
            break;
        }
    }

    data += strlen(data) + 1 + MMB_LEN_SERVER_STATUS;

    return data;
}

/**
 * Process a master
 */
const char* process_master(Monitor* monitor, MonitorServer** master,
                           const char* data, const char* end)
{
    if (master)
    {
        for (MonitorServer* db : monitor->m_servers)
        {
            if (strcmp(db->server->name(), data) == 0)
            {
                *master = db;
                break;
            }
        }
    }

    data += strlen(data) + 1;

    return data;
}

/**
 * Check that the calculated CRC32 matches the one stored on disk
 */
bool check_crc32(const uint8_t* data, uint32_t size, const uint8_t* crc_ptr)
{
    uint32_t crc = mxs_get_byte4(crc_ptr);
    uint32_t calculated_crc = crc32(0L, NULL, 0);
    calculated_crc = crc32(calculated_crc, data, size);
    return calculated_crc == crc;
}

/**
 * Process the stored journal data
 */
bool process_data_file(Monitor* monitor, MonitorServer** master,
                       const char* data, const char* crc_ptr)
{
    const char* ptr = data;
    MXB_AT_DEBUG(const char* prevptr = ptr);

    while (ptr < crc_ptr)
    {
        /** All values contain a null terminated string */
        if (!has_null_terminator(ptr, crc_ptr))
        {
            MXS_ERROR("Possible corrupted journal file (no null terminator found). Ignoring.");
            return false;
        }

        stored_value_type type = (stored_value_type)ptr[0];
        ptr += MMB_LEN_VALUE_TYPE;

        switch (type)
        {
        case SVT_SERVER:
            ptr = process_server(monitor, ptr, crc_ptr);
            break;

        case SVT_MASTER:
            ptr = process_master(monitor, master, ptr, crc_ptr);
            break;

        default:
            MXS_ERROR("Possible corrupted journal file (unknown stored value). Ignoring.");
            return false;
        }
        mxb_assert(prevptr != ptr);
        MXB_AT_DEBUG(prevptr = ptr);
    }

    mxb_assert(ptr == crc_ptr);
    return true;
}

bool check_disk_space_exhausted(MonitorServer* pMs,
                                const std::string& path,
                                const maxscale::disk::SizesAndName& san,
                                int32_t max_percentage)
{
    bool disk_space_exhausted = false;

    int32_t used_percentage = ((san.total() - san.available()) / (double)san.total()) * 100;

    if (used_percentage >= max_percentage)
    {
        MXS_ERROR("Disk space on %s at %s is exhausted; %d%% of the the disk "
                  "mounted on the path %s has been used, and the limit it %d%%.",
                  pMs->server->name(),
                  pMs->server->address,
                  used_percentage,
                  path.c_str(),
                  max_percentage);
        disk_space_exhausted = true;
    }

    return disk_space_exhausted;
}

const char ERR_CANNOT_MODIFY[] =
    "The server is monitored, so only the maintenance status can be "
    "set/cleared manually. Status was not modified.";
const char WRN_REQUEST_OVERWRITTEN[] =
    "Previous maintenance request was not yet read by the monitor and was overwritten.";
}

namespace maxscale
{

Monitor::Monitor(const string& name, const string& module)
    : m_name(name)
    , m_module(module)
{
    memset(m_journal_hash, 0, sizeof(m_journal_hash));
}

void Monitor::stop()
{
    do_stop();

    for (auto db : m_servers)
    {
        // TODO: Should be db->close().
        mysql_close(db->con);
        db->con = NULL;
    }
}

const char* Monitor::name() const
{
    return m_name.c_str();
}

using std::chrono::milliseconds;
using std::chrono::seconds;

bool Monitor::configure(const MXS_CONFIG_PARAMETER* params)
{
    m_settings.interval = params->get_duration<milliseconds>(CN_MONITOR_INTERVAL).count();
    m_settings.journal_max_age = params->get_duration<seconds>(CN_JOURNAL_MAX_AGE).count();
    m_settings.script_timeout = params->get_duration<seconds>(CN_SCRIPT_TIMEOUT).count();
    m_settings.script = params->get_string(CN_SCRIPT);
    m_settings.events = params->get_enum(CN_EVENTS, mxs_monitor_event_enum_values);

    MonitorServer::ConnectionSettings& conn_settings = m_settings.conn_settings;
    conn_settings.read_timeout = params->get_duration<seconds>(CN_BACKEND_READ_TIMEOUT).count();
    conn_settings.write_timeout = params->get_duration<seconds>(CN_BACKEND_WRITE_TIMEOUT).count();
    conn_settings.connect_timeout = params->get_duration<seconds>(CN_BACKEND_CONNECT_TIMEOUT).count();
    conn_settings.connect_attempts = params->get_integer(CN_BACKEND_CONNECT_ATTEMPTS);
    conn_settings.username = params->get_string(CN_USER);
    conn_settings.password = params->get_string(CN_PASSWORD);

    // Disk check interval is given in ms, duration is constructed from seconds.
    auto dsc_interval = params->get_duration<milliseconds>(CN_DISK_SPACE_CHECK_INTERVAL).count();
    // 0 implies disabling -> save negative value to interval.
    m_settings.disk_space_check_interval = (dsc_interval > 0) ?
        mxb::Duration(static_cast<double>(dsc_interval) / 1000) : mxb::Duration(-1);

    // The monitor serverlist has already been checked to be valid. Empty value is ok too.
    // First, remove all servers.
    remove_all_servers();

    auto servers_temp = params->get_server_list(CN_SERVERS);
    bool error = false;
    for (auto elem : servers_temp)
    {
        if (!add_server(elem))
        {
            error = true;
        }
    }

    /* The previous config values were normal types and were checked by the config manager
     * to be correct. The following is a complicated type and needs to be checked separately. */
    auto threshold_string = params->get_string(CN_DISK_SPACE_THRESHOLD);
    if (!set_disk_space_threshold(threshold_string))
    {
        MXS_ERROR("Invalid value for '%s' for monitor %s: %s",
                  CN_DISK_SPACE_THRESHOLD, name(), threshold_string.c_str());
        error = true;
    }

    if (!error)
    {
        // Store module name into parameter storage.
        m_parameters.set(CN_MODULE, m_module);
        // Add all config settings to text-mode storage. Needed for serialization.
        m_parameters.set_multiple(*params);
    }
    return !error;
}

const MXS_CONFIG_PARAMETER& Monitor::parameters() const
{
    return m_parameters;
}

const Monitor::Settings& Monitor::settings() const
{
    return m_settings;
}

long Monitor::ticks() const
{
    return m_ticks.load(std::memory_order_acquire);
}

Monitor::~Monitor()
{
    for (auto server : m_servers)
    {
        // TODO: store unique pointers in the array
        delete server;
    }
    m_servers.clear();
}

/**
 * Add a server to the monitor. Fails if server is already monitored.
 *
 * @param server  A server
 * @return True if server was added
 */
bool Monitor::add_server(SERVER* server)
{
    // This should only be called from the admin thread while the monitor is stopped.
    mxb_assert(state() == MONITOR_STATE_STOPPED && is_admin_thread());
    bool success = false;
    string existing_owner;
    if (this_unit.claim_server(server->name(), m_name, &existing_owner))
    {
        auto new_server = new MonitorServer(server, m_settings.disk_space_limits);
        m_servers.push_back(new_server);
        server_added(server);
        success = true;
    }
    else
    {
        MXS_ERROR("Server '%s' is already monitored by '%s', cannot add it to another monitor.",
                  server->name(), existing_owner.c_str());
    }
    return success;
}

void Monitor::server_added(SERVER* server)
{
    service_add_server(this, server);
}

void Monitor::server_removed(SERVER* server)
{
    service_remove_server(this, server);
}

/**
 * Remove all servers from the monitor.
 */
void Monitor::remove_all_servers()
{
    // This should only be called from the admin thread while the monitor is stopped.
    mxb_assert(state() == MONITOR_STATE_STOPPED && is_admin_thread());
    for (auto mon_server : m_servers)
    {
        mxb_assert(this_unit.claimed_by(mon_server->server->name()) == m_name);
        this_unit.release_server(mon_server->server->name());
        server_removed(mon_server->server);
        delete mon_server;
    }
    m_servers.clear();
}

void Monitor::show(DCB* dcb)
{
    dcb_printf(dcb, "Name:                   %s\n", name());
    dcb_printf(dcb, "State:                  %s\n", monitor_state_to_string(state()));
    dcb_printf(dcb, "Times monitored:        %li\n", ticks());
    dcb_printf(dcb, "Sampling interval:      %lu milliseconds\n", m_settings.interval);
    dcb_printf(dcb, "Connect Timeout:        %i seconds\n", m_settings.conn_settings.connect_timeout);
    dcb_printf(dcb, "Read Timeout:           %i seconds\n", m_settings.conn_settings.read_timeout);
    dcb_printf(dcb, "Write Timeout:          %i seconds\n", m_settings.conn_settings.write_timeout);
    dcb_printf(dcb, "Connect attempts:       %i \n", m_settings.conn_settings.connect_attempts);
    dcb_printf(dcb, "Monitored servers:      ");

    const char* sep = "";

    for (const auto& db : m_servers)
    {
        dcb_printf(dcb, "%s[%s]:%d", sep, db->server->address, db->server->port);
        sep = ", ";
    }

    dcb_printf(dcb, "\n");

    if (state() == MONITOR_STATE_RUNNING)
    {
        diagnostics(dcb);
    }
    else
    {
        dcb_printf(dcb, " (no diagnostics)\n");
    }
    dcb_printf(dcb, "\n");
}

json_t* Monitor::to_json(const char* host) const
{
    // This function mostly reads settings-type data, which is only written to by the admin thread,
    // The rest is safe to read without mutexes.
    mxb_assert(Monitor::is_admin_thread());
    json_t* rval = json_object();
    json_t* attr = json_object();
    json_t* rel = json_object();

    auto my_name = name();
    json_object_set_new(rval, CN_ID, json_string(my_name));
    json_object_set_new(rval, CN_TYPE, json_string(CN_MONITORS));

    json_object_set_new(attr, CN_MODULE, json_string(m_module.c_str()));
    auto my_state = state();
    json_object_set_new(attr, CN_STATE, json_string(monitor_state_to_string(my_state)));
    json_object_set_new(attr, CN_TICKS, json_integer(ticks()));

    /** Monitor parameters */
    json_object_set_new(attr, CN_PARAMETERS, parameters_to_json());

    if (my_state == MONITOR_STATE_RUNNING)
    {
        json_t* diag = diagnostics_json();
        if (diag)
        {
            json_object_set_new(attr, CN_MONITOR_DIAGNOSTICS, diag);
        }
    }

    if (!m_servers.empty())
    {
        json_t* mon_rel = mxs_json_relationship(host, MXS_JSON_API_SERVERS);
        for (MonitorServer* db : m_servers)
        {
            mxs_json_add_relation(mon_rel, db->server->name(), CN_SERVERS);
        }
        json_object_set_new(rel, CN_SERVERS, mon_rel);
    }

    json_object_set_new(rval, CN_RELATIONSHIPS, rel);
    json_object_set_new(rval, CN_ATTRIBUTES, attr);
    json_object_set_new(rval, CN_LINKS, mxs_json_self_link(host, CN_MONITORS, my_name));
    return rval;
}

json_t* Monitor::parameters_to_json() const
{
    json_t* rval = json_object();
    const MXS_MODULE* mod = get_module(m_module.c_str(), MODULE_MONITOR);
    auto  my_config = parameters();
    config_add_module_params_json(&my_config,
                                  {CN_TYPE, CN_MODULE, CN_SERVERS},
                                  config_monitor_params,
                                  mod->parameters,
                                  rval);
    return rval;
}

bool Monitor::test_permissions(const string& query)
{
    auto monitor = this;
    if (monitor->m_servers.empty() || config_get_global_options()->skip_permission_checks)
    {
        return true;
    }

    char* dpasswd = decrypt_password(m_settings.conn_settings.password.c_str());
    bool rval = false;

    for (MonitorServer* mondb : monitor->m_servers)
    {
        if (!connection_is_ok(mondb->ping_or_connect(m_settings.conn_settings)))
        {
            MXS_ERROR("[%s] Failed to connect to server '%s' ([%s]:%d) when"
                      " checking monitor user credentials and permissions: %s",
                      monitor->name(),
                      mondb->server->name(),
                      mondb->server->address,
                      mondb->server->port,
                      mysql_error(mondb->con));
            switch (mysql_errno(mondb->con))
            {
            case ER_ACCESS_DENIED_ERROR:
            case ER_DBACCESS_DENIED_ERROR:
            case ER_ACCESS_DENIED_NO_PASSWORD_ERROR:
                break;

            default:
                rval = true;
                break;
            }
        }
        else if (mxs_mysql_query(mondb->con, query.c_str()) != 0)
        {
            switch (mysql_errno(mondb->con))
            {
            case ER_TABLEACCESS_DENIED_ERROR:
            case ER_COLUMNACCESS_DENIED_ERROR:
            case ER_SPECIFIC_ACCESS_DENIED_ERROR:
            case ER_PROCACCESS_DENIED_ERROR:
            case ER_KILL_DENIED_ERROR:
                rval = false;
                break;

            default:
                rval = true;
                break;
            }

            MXS_ERROR("[%s] Failed to execute query '%s' with user '%s'. MySQL error message: %s",
                      name(), query.c_str(), m_settings.conn_settings.username.c_str(),
                      mysql_error(mondb->con));
        }
        else
        {
            rval = true;
            MYSQL_RES* res = mysql_use_result(mondb->con);
            if (res == NULL)
            {
                MXS_ERROR("[%s] Result retrieval failed when checking monitor permissions: %s",
                          monitor->name(),
                          mysql_error(mondb->con));
            }
            else
            {
                mysql_free_result(res);
            }
        }
    }

    MXS_FREE(dpasswd);
    return rval;
}

void MonitorServer::stash_current_status()
{
    mon_prev_status = server->status;
    pending_status = server->status;
}

void MonitorServer::set_pending_status(uint64_t bits)
{
    pending_status |= bits;
}

void MonitorServer::clear_pending_status(uint64_t bits)
{
    pending_status &= ~bits;
}

/*
 * Determine a monitor event, defined by the difference between the old
 * status of a server and the new status.
 *
 * @return  monitor_event_t     A monitor event (enum)
 *
 * @note This function must only be called from mon_process_state_changes
 */
mxs_monitor_event_t MonitorServer::get_event_type() const
{
    typedef enum
    {
        DOWN_EVENT,
        UP_EVENT,
        LOSS_EVENT,
        NEW_EVENT,
        UNSUPPORTED_EVENT
    } general_event_type;

    general_event_type event_type = UNSUPPORTED_EVENT;

    uint64_t prev = mon_prev_status & all_server_bits;
    uint64_t present = server->status & all_server_bits;

    if (prev == present)
    {
        /* This should never happen */
        mxb_assert(false);
        return UNDEFINED_EVENT;
    }

    if ((prev & SERVER_RUNNING) == 0)
    {
        /* The server was not running previously */
        if ((present & SERVER_RUNNING) != 0)
        {
            event_type = UP_EVENT;
        }
        else
        {
            /* Otherwise, was not running and still is not running. This should never happen. */
            mxb_assert(false);
        }
    }
    else
    {
        /* Previous state must have been running */
        if ((present & SERVER_RUNNING) == 0)
        {
            event_type = DOWN_EVENT;
        }
        else
        {
            /** These are used to detect whether we actually lost something or
             * just transitioned from one state to another */
            uint64_t prev_bits = prev & (SERVER_MASTER | SERVER_SLAVE);
            uint64_t present_bits = present & (SERVER_MASTER | SERVER_SLAVE);

            /* Was running and still is */
            if ((!prev_bits || !present_bits || prev_bits == present_bits)
                && (prev & server_type_bits))
            {
                /* We used to know what kind of server it was */
                event_type = LOSS_EVENT;
            }
            else
            {
                /* We didn't know what kind of server it was, now we do*/
                event_type = NEW_EVENT;
            }
        }
    }

    mxs_monitor_event_t rval = UNDEFINED_EVENT;

    switch (event_type)
    {
    case UP_EVENT:
        rval = (present & SERVER_MASTER) ? MASTER_UP_EVENT :
            (present & SERVER_SLAVE) ? SLAVE_UP_EVENT :
            (present & SERVER_JOINED) ? SYNCED_UP_EVENT :
            SERVER_UP_EVENT;
        break;

    case DOWN_EVENT:
        rval = (prev & SERVER_MASTER) ? MASTER_DOWN_EVENT :
            (prev & SERVER_SLAVE) ? SLAVE_DOWN_EVENT :
            (prev & SERVER_JOINED) ? SYNCED_DOWN_EVENT :
            SERVER_DOWN_EVENT;
        break;

    case LOSS_EVENT:
        rval = (prev & SERVER_MASTER) ? LOST_MASTER_EVENT :
            (prev & SERVER_SLAVE) ? LOST_SLAVE_EVENT :
            (prev & SERVER_JOINED) ? LOST_SYNCED_EVENT :
            UNDEFINED_EVENT;
        break;

    case NEW_EVENT:
        rval = (present & SERVER_MASTER) ? NEW_MASTER_EVENT :
            (present & SERVER_SLAVE) ? NEW_SLAVE_EVENT :
            (present & SERVER_JOINED) ? NEW_SYNCED_EVENT :
            UNDEFINED_EVENT;
        break;

    default:
        /* This should never happen */
        mxb_assert(false);
        break;
    }

    mxb_assert(rval != UNDEFINED_EVENT);
    return rval;
}

const char* Monitor::get_event_name(mxs_monitor_event_t event)
{
    for (int i = 0; mxs_monitor_event_enum_values[i].name; i++)
    {
        if (mxs_monitor_event_enum_values[i].enum_value == event)
        {
            return mxs_monitor_event_enum_values[i].name;
        }
    }

    mxb_assert(!true);
    return "undefined_event";
}

const char* MonitorServer::get_event_name()
{
    return Monitor::get_event_name((mxs_monitor_event_t) server->last_event);
}

void Monitor::append_node_names(char* dest, int len, int status, credentials_approach_t approach)
{
    const char* separator = "";
    // Some extra space for port and separator
    char arr[SERVER::MAX_MONUSER_LEN + SERVER::MAX_MONPW_LEN + SERVER::MAX_ADDRESS_LEN + 64];
    dest[0] = '\0';

    for (auto iter = m_servers.begin(); iter != m_servers.end() && len; ++iter)
    {
        Server* server = static_cast<Server*>((*iter)->server);
        if (status == 0 || server->status & status)
        {
            if (approach == CREDENTIALS_EXCLUDE)
            {
                snprintf(arr,
                         sizeof(arr),
                         "%s[%s]:%d",
                         separator,
                         server->address,
                         server->port);
            }
            else
            {
                string user = m_settings.conn_settings.username;
                string password = m_settings.conn_settings.password;
                string server_specific_monuser = server->monitor_user();
                if (!server_specific_monuser.empty())
                {
                    user = server_specific_monuser;
                    password = server->monitor_password();
                }

                snprintf(arr,
                         sizeof(arr),
                         "%s%s:%s@[%s]:%d",
                         separator,
                         user.c_str(),
                         password.c_str(),
                         server->address,
                         server->port);
            }

            separator = ",";
            int arrlen = strlen(arr);

            if (arrlen < len)
            {
                strcat(dest, arr);
                len -= arrlen;
            }
        }
    }
}

/**
 * Check if current monitored server status has changed.
 *
 * @return              true if status has changed
 */
bool MonitorServer::status_changed()
{
    bool rval = false;

    /* Previous status is -1 if not yet set */
    if (mon_prev_status != static_cast<uint64_t>(-1))
    {

        uint64_t old_status = mon_prev_status & all_server_bits;
        uint64_t new_status = server->status & all_server_bits;

        /**
         * The state has changed if the relevant state bits are not the same,
         * the server is either running, stopping or starting and the server is
         * not going into maintenance or coming out of it
         */
        if (old_status != new_status
            && ((old_status | new_status) & SERVER_MAINT) == 0
            && ((old_status | new_status) & SERVER_RUNNING) == SERVER_RUNNING)
        {
            rval = true;
        }
    }

    return rval;
}

/**
 * Check if current monitored server has a loggable failure status.
 *
 * @return true if failed status can be logged or false
 */
bool MonitorServer::should_print_fail_status()
{
    return server->is_down() && mon_err_count == 0;
}

MonitorServer* Monitor::find_parent_node(MonitorServer* target)
{
    MonitorServer* rval = NULL;

    if (target->server->master_id > 0)
    {
        for (MonitorServer* node : m_servers)
        {
            if (node->server->node_id == target->server->master_id)
            {
                rval = node;
                break;
            }
        }
    }

    return rval;
}

std::string Monitor::child_nodes(MonitorServer* parent)
{
    std::stringstream ss;

    if (parent->server->node_id > 0)
    {
        bool have_content = false;
        for (MonitorServer* node : m_servers)
        {
            if (node->server->master_id == parent->server->node_id)
            {
                if (have_content)
                {
                    ss << ",";
                }

                ss << "[" << node->server->address << "]:" << node->server->port;
                have_content = true;
            }
        }
    }

    return ss.str();
}

int Monitor::launch_command(MonitorServer* ptr, EXTERNCMD* cmd)
{
    if (externcmd_matches(cmd, "$INITIATOR"))
    {
        char initiator[strlen(ptr->server->address) + 24];      // Extra space for port
        snprintf(initiator, sizeof(initiator), "[%s]:%d", ptr->server->address, ptr->server->port);
        externcmd_substitute_arg(cmd, "[$]INITIATOR", initiator);
    }

    if (externcmd_matches(cmd, "$PARENT"))
    {
        std::stringstream ss;
        MonitorServer* parent = find_parent_node(ptr);

        if (parent)
        {
            ss << "[" << parent->server->address << "]:" << parent->server->port;
        }
        externcmd_substitute_arg(cmd, "[$]PARENT", ss.str().c_str());
    }

    if (externcmd_matches(cmd, "$CHILDREN"))
    {
        externcmd_substitute_arg(cmd, "[$]CHILDREN", child_nodes(ptr).c_str());
    }

    if (externcmd_matches(cmd, "$EVENT"))
    {
        externcmd_substitute_arg(cmd, "[$]EVENT", ptr->get_event_name());
    }

    char nodelist[PATH_MAX + MON_ARG_MAX + 1] = {'\0'};

    if (externcmd_matches(cmd, "$CREDENTIALS"))
    {
        // We provide the credentials for _all_ servers.
        append_node_names(nodelist, sizeof(nodelist), 0, CREDENTIALS_INCLUDE);
        externcmd_substitute_arg(cmd, "[$]CREDENTIALS", nodelist);
    }

    if (externcmd_matches(cmd, "$NODELIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_RUNNING);
        externcmd_substitute_arg(cmd, "[$]NODELIST", nodelist);
    }

    if (externcmd_matches(cmd, "$LIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), 0);
        externcmd_substitute_arg(cmd, "[$]LIST", nodelist);
    }

    if (externcmd_matches(cmd, "$MASTERLIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_MASTER);
        externcmd_substitute_arg(cmd, "[$]MASTERLIST", nodelist);
    }

    if (externcmd_matches(cmd, "$SLAVELIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_SLAVE);
        externcmd_substitute_arg(cmd, "[$]SLAVELIST", nodelist);
    }

    if (externcmd_matches(cmd, "$SYNCEDLIST"))
    {
        append_node_names(nodelist, sizeof(nodelist), SERVER_JOINED);
        externcmd_substitute_arg(cmd, "[$]SYNCEDLIST", nodelist);
    }

    int rv = externcmd_execute(cmd);

    if (rv)
    {
        if (rv == -1)
        {
            // Internal error
            MXS_ERROR("Failed to execute script '%s' on server state change event '%s'",
                      cmd->argv[0],
                      ptr->get_event_name());
        }
        else
        {
            // Script returned a non-zero value
            MXS_ERROR("Script '%s' returned %d on event '%s'",
                      cmd->argv[0],
                      rv,
                      ptr->get_event_name());
        }
    }
    else
    {
        mxb_assert(cmd->argv != NULL && cmd->argv[0] != NULL);
        // Construct a string with the script + arguments
        char* scriptStr = NULL;
        int totalStrLen = 0;
        bool memError = false;
        for (int i = 0; cmd->argv[i]; i++)
        {
            totalStrLen += strlen(cmd->argv[i]) + 1;    // +1 for space and one \0
        }
        int spaceRemaining = totalStrLen;
        if ((scriptStr = (char*)MXS_CALLOC(totalStrLen, sizeof(char))) != NULL)
        {
            char* currentPos = scriptStr;
            // The script name should not begin with a space
            int len = snprintf(currentPos, spaceRemaining, "%s", cmd->argv[0]);
            currentPos += len;
            spaceRemaining -= len;

            for (int i = 1; cmd->argv[i]; i++)
            {
                if ((cmd->argv[i])[0] == '\0')
                {
                    continue;   // Empty argument, print nothing
                }
                len = snprintf(currentPos, spaceRemaining, " %s", cmd->argv[i]);
                currentPos += len;
                spaceRemaining -= len;
            }
            mxb_assert(spaceRemaining > 0);
            *currentPos = '\0';
        }
        else
        {
            memError = true;
            scriptStr = cmd->argv[0];   // print at least something
        }

        MXS_NOTICE("Executed monitor script '%s' on event '%s'",
                   scriptStr,
                   ptr->get_event_name());

        if (!memError)
        {
            MXS_FREE(scriptStr);
        }
    }

    return rv;
}

int Monitor::launch_script(MonitorServer* ptr)
{
    const char* script = m_settings.script.c_str();
    char arg[strlen(script) + 1];
    strcpy(arg, script);

    EXTERNCMD* cmd = externcmd_allocate(arg, m_settings.script_timeout);

    if (cmd == NULL)
    {
        MXS_ERROR("Failed to initialize script '%s'. See previous errors for the "
                  "cause of this failure.",
                  script);
        return -1;
    }

    int rv = launch_command(ptr, cmd);

    externcmd_free(cmd);

    return rv;
}

mxs_connect_result_t Monitor::ping_or_connect_to_db(const MonitorServer::ConnectionSettings& sett,
                                                    SERVER& server, MYSQL** ppConn)
{
    mxb_assert(ppConn);
    auto pConn = *ppConn;
    if (pConn)
    {
        /** Return if the connection is OK */
        if (mysql_ping(pConn) == 0)
        {
            return MONITOR_CONN_EXISTING_OK;
        }
        /** Otherwise close the handle. */
        mysql_close(pConn);
    }

    mxs_connect_result_t conn_result = MONITOR_CONN_REFUSED;
    if ((pConn = mysql_init(NULL)) != nullptr)
    {
        string uname = sett.username;
        string passwd = sett.password;
        const Server& srv = static_cast<const Server&>(server);     // Clean this up later.
        string server_specific_monuser = srv.monitor_user();
        if (!server_specific_monuser.empty())
        {
            uname = server_specific_monuser;
            passwd = srv.monitor_password();
        }
        char* dpwd = decrypt_password(passwd.c_str());

        mysql_optionsv(pConn, MYSQL_OPT_CONNECT_TIMEOUT, &sett.connect_timeout);
        mysql_optionsv(pConn, MYSQL_OPT_READ_TIMEOUT, &sett.read_timeout);
        mysql_optionsv(pConn, MYSQL_OPT_WRITE_TIMEOUT, &sett.write_timeout);
        mysql_optionsv(pConn, MYSQL_PLUGIN_DIR, get_connector_plugindir());

        time_t start = 0;
        time_t end = 0;
        for (int i = 0; i < sett.connect_attempts; i++)
        {
            start = time(NULL);
            bool result = (mxs_mysql_real_connect(pConn, &server, uname.c_str(), dpwd) != NULL);
            end = time(NULL);

            if (result)
            {
                conn_result = MONITOR_CONN_NEWCONN_OK;
                break;
            }
        }

        if (conn_result == MONITOR_CONN_REFUSED && difftime(end, start) >= sett.connect_timeout)
        {
            conn_result = MONITOR_CONN_TIMEOUT;
        }
        MXS_FREE(dpwd);
    }

    *ppConn = pConn;
    return conn_result;
}

mxs_connect_result_t MonitorServer::ping_or_connect(const ConnectionSettings& settings)
{
    return Monitor::ping_or_connect_to_db(settings, *server, &con);
}

/**
 * Is the return value one of the 'OK' values.
 *
 * @param connect_result Return value of mon_ping_or_connect_to_db
 * @return True of connection is ok
 */
bool Monitor::connection_is_ok(mxs_connect_result_t connect_result)
{
    return connect_result == MONITOR_CONN_EXISTING_OK || connect_result == MONITOR_CONN_NEWCONN_OK;
}

string Monitor::get_server_monitor(const SERVER* server)
{
    return this_unit.claimed_by(server->name());
}

bool Monitor::is_admin_thread()
{
    mxb::Worker* current = mxb::Worker::get_current();
    return current == nullptr || current == mxs_rworker_get(MXS_RWORKER_MAIN);
}

/**
 * Log an error about the failure to connect to a backend server and why it happened.
 *
 * @param rval Return value of mon_ping_or_connect_to_db
 */
void MonitorServer::log_connect_error(mxs_connect_result_t rval)
{
    mxb_assert(!Monitor::connection_is_ok(rval));
    const char TIMED_OUT[] = "Monitor timed out when connecting to server %s[%s:%d] : '%s'";
    const char REFUSED[] = "Monitor was unable to connect to server %s[%s:%d] : '%s'";
    MXS_ERROR(rval == MONITOR_CONN_TIMEOUT ? TIMED_OUT : REFUSED,
              server->name(),
              server->address,
              server->port,
              mysql_error(con));
}

void MonitorServer::log_state_change()
{
    string prev = SERVER::status_to_string(mon_prev_status);
    string next = server->status_string();
    MXS_NOTICE("Server changed state: %s[%s:%u]: %s. [%s] -> [%s]",
               server->name(), server->address, server->port,
               get_event_name(),
               prev.c_str(), next.c_str());
}

void Monitor::hangup_failed_servers()
{
    for (MonitorServer* ptr : m_servers)
    {
        if (ptr->status_changed() && (!(ptr->server->is_usable()) || !(ptr->server->is_in_cluster())))
        {
            dcb_hangup_foreach(ptr->server);
        }
    }
}

void MonitorServer::mon_report_query_error()
{
    MXS_ERROR("Failed to execute query on server '%s' ([%s]:%d): %s",
              server->name(),
              server->address,
              server->port,
              mysql_error(con));
}

/**
 * Check if admin is requesting setting or clearing maintenance status on the server and act accordingly.
 * Should be called at the beginning of a monitor loop.
 */
void Monitor::check_maintenance_requests()
{
    /* In theory, the admin may be modifying the server maintenance status during this function. The overall
     * maintenance flag should be read-written atomically to prevent missing a value. */
    bool was_pending = m_status_change_pending.exchange(false, std::memory_order_acq_rel);
    if (was_pending)
    {
        for (auto ptr : m_servers)
        {
            // The admin can only modify the [Maintenance] and [Drain] bits.
            int admin_msg = atomic_exchange_int(&ptr->status_request, MonitorServer::NO_CHANGE);

            switch (admin_msg)
            {
            case MonitorServer::MAINT_ON:
                ptr->server->set_status(SERVER_MAINT);
                break;

            case MonitorServer::MAINT_OFF:
                ptr->server->clear_status(SERVER_MAINT);
                break;

            case MonitorServer::BEING_DRAINED_ON:
                ptr->server->set_status(SERVER_DRAINING);
                break;

            case MonitorServer::BEING_DRAINED_OFF:
                ptr->server->clear_status(SERVER_DRAINING);
                break;

            case MonitorServer::NO_CHANGE:
                break;

            default:
                mxb_assert(!true);
            }
        }
    }
}

void Monitor::detect_handle_state_changes()
{
    bool master_down = false;
    bool master_up = false;

    for (MonitorServer* ptr : m_servers)
    {
        if (ptr->status_changed())
        {
            /**
             * The last executed event will be needed if a passive MaxScale is
             * promoted to an active one and the last event that occurred on
             * a server was a master_down event.
             *
             * In this case, a failover script should be called if no master_up
             * or new_master events are triggered within a pre-defined time limit.
             */
            mxs_monitor_event_t event = ptr->get_event_type();
            ptr->server->last_event = event;
            ptr->server->triggered_at = mxs_clock();
            ptr->log_state_change();

            if (event == MASTER_DOWN_EVENT)
            {
                master_down = true;
            }
            else if (event == MASTER_UP_EVENT || event == NEW_MASTER_EVENT)
            {
                master_up = true;
            }

            if (!m_settings.script.empty() && (event & m_settings.events))
            {
                launch_script(ptr);
            }
        }
    }

    if (master_down && master_up)
    {
        MXS_NOTICE("Master switch detected: lost a master and gained a new one");
    }
}

int Monitor::get_data_file_path(char* path) const
{
    int rv = snprintf(path, PATH_MAX, journal_template, get_datadir(), name(), journal_name);
    return rv;
}

/**
 * @brief Open stored journal file
 *
 * @param monitor Monitor to reload
 * @param path Output where path is stored
 * @return Opened file or NULL on error
 */
FILE* Monitor::open_data_file(Monitor* monitor, char* path)
{
    FILE* rval = NULL;
    int nbytes = monitor->get_data_file_path(path);

    if (nbytes < PATH_MAX)
    {
        if ((rval = fopen(path, "rb")) == NULL && errno != ENOENT)
        {
            MXS_ERROR("Failed to open journal file: %d, %s", errno, mxs_strerror(errno));
        }
    }
    else
    {
        MXS_ERROR("Path is too long: %d characters exceeds the maximum path "
                  "length of %d bytes",
                  nbytes,
                  PATH_MAX);
    }

    return rval;
}

void Monitor::store_server_journal(MonitorServer* master)
{
    auto monitor = this;    // TODO: cleanup later
    /** Calculate how much memory we need to allocate */
    uint32_t size = MMB_LEN_SCHEMA_VERSION + MMB_LEN_CRC32;

    for (MonitorServer* db : m_servers)
    {
        /** Each server is stored as a type byte and a null-terminated string
         * followed by eight byte server status. */
        size += MMB_LEN_VALUE_TYPE + strlen(db->server->name()) + 1 + MMB_LEN_SERVER_STATUS;
    }

    if (master)
    {
        /** The master server name is stored as a null terminated string */
        size += MMB_LEN_VALUE_TYPE + strlen(master->server->name()) + 1;
    }

    /** 4 bytes for file length, 1 byte for schema version and 4 bytes for CRC32 */
    uint32_t buffer_size = size + MMB_LEN_BYTES;
    uint8_t* data = (uint8_t*)MXS_MALLOC(buffer_size);
    char path[PATH_MAX + 1];

    if (data)
    {
        /** Store the data in memory first and compare the current hash to
         * the hash of the last stored journal. This isn't a fool-proof
         * method of detecting changes but any failures are mainly of
         * theoretical nature. */
        store_data(monitor, master, data, size);
        uint8_t hash[SHA_DIGEST_LENGTH];
        SHA1(data, size, hash);

        if (memcmp(monitor->m_journal_hash, hash, sizeof(hash)) != 0)
        {
            FILE* file = open_tmp_file(monitor, path);

            if (file)
            {
                /** Write the data to a temp file and rename it to the final name */
                if (fwrite(data, 1, buffer_size, file) == buffer_size && fflush(file) == 0)
                {
                    if (!rename_tmp_file(monitor, path))
                    {
                        unlink(path);
                    }
                    else
                    {
                        memcpy(monitor->m_journal_hash, hash, sizeof(hash));
                    }
                }
                else
                {
                    MXS_ERROR("Failed to write journal data to disk: %d, %s",
                              errno,
                              mxs_strerror(errno));
                }
                fclose(file);
            }
        }
    }
    MXS_FREE(data);
}

void Monitor::load_server_journal(MonitorServer** master)
{
    auto monitor = this;    // TODO: cleanup later
    char path[PATH_MAX];
    FILE* file = open_data_file(monitor, path);

    if (file)
    {
        uint32_t size = 0;
        size_t bytes = fread(&size, 1, MMB_LEN_BYTES, file);
        mxb_assert(sizeof(size) == MMB_LEN_BYTES);

        if (bytes == MMB_LEN_BYTES)
        {
            /** Payload contents:
             *
             * - One byte of schema version
             * - `size - 5` bytes of data
             * - Trailing 4 bytes of CRC32
             */
            char* data = (char*)MXS_MALLOC(size);

            if (data && (bytes = fread(data, 1, size, file)) == size)
            {
                if (*data == MMB_SCHEMA_VERSION)
                {
                    if (check_crc32((uint8_t*)data,
                                    size - MMB_LEN_CRC32,
                                    (uint8_t*)data + size - MMB_LEN_CRC32))
                    {
                        if (process_data_file(monitor,
                                              master,
                                              data + MMB_LEN_SCHEMA_VERSION,
                                              data + size - MMB_LEN_CRC32))
                        {
                            MXS_NOTICE("Loaded server states from journal file: %s", path);
                        }
                    }
                    else
                    {
                        MXS_ERROR("CRC32 mismatch in journal file. Ignoring.");
                    }
                }
                else
                {
                    MXS_ERROR("Unknown journal schema version: %d", (int)*data);
                }
            }
            else if (data)
            {
                if (ferror(file))
                {
                    MXS_ERROR("Failed to read journal file: %d, %s", errno, mxs_strerror(errno));
                }
                else
                {
                    MXS_ERROR("Failed to read journal file: Expected %u bytes, "
                              "read %lu bytes.",
                              size,
                              bytes);
                }
            }
            MXS_FREE(data);
        }
        else
        {
            if (ferror(file))
            {
                MXS_ERROR("Failed to read journal file length: %d, %s",
                          errno,
                          mxs_strerror(errno));
            }
            else
            {
                MXS_ERROR("Failed to read journal file length: Expected %d bytes, "
                          "read %lu bytes.",
                          MMB_LEN_BYTES,
                          bytes);
            }
        }

        fclose(file);
    }
}

void Monitor::remove_server_journal()
{
    char path[PATH_MAX];
    if (get_data_file_path(path) < PATH_MAX)
    {
        unlink(path);
    }
    else
    {
        MXS_ERROR("Path to monitor journal directory is too long.");
    }
}

bool Monitor::journal_is_stale() const
{
    bool is_stale = true;
    char path[PATH_MAX];
    auto max_age = m_settings.journal_max_age;
    if (get_data_file_path(path) < PATH_MAX)
    {
        struct stat st;

        if (stat(path, &st) == 0)
        {
            time_t tdiff = time(NULL) - st.st_mtim.tv_sec;

            if (tdiff >= max_age)
            {
                MXS_WARNING("Journal file was created %ld seconds ago. Maximum journal "
                            "age is %ld seconds.",
                            tdiff,
                            max_age);
            }
            else
            {
                is_stale = false;
            }
        }
        else if (errno != ENOENT)
        {
            MXS_ERROR("Failed to inspect journal file: %d, %s", errno, mxs_strerror(errno));
        }
    }
    else
    {
        MXS_ERROR("Path to monitor journal directory is too long.");
    }

    return is_stale;
}

MonitorServer* Monitor::get_monitored_server(SERVER* search_server)
{
    mxb_assert(search_server);
    for (const auto iter : m_servers)
    {
        if (iter->server == search_server)
        {
            return iter;
        }
    }
    return nullptr;
}

std::vector<MonitorServer*> Monitor::get_monitored_serverlist(const string& key, bool* error_out)
{
    std::vector<MonitorServer*> monitored_array;
    // Check that value exists.
    if (!m_parameters.contains(key))
    {
        return monitored_array;
    }

    string name_error;
    auto servers = m_parameters.get_server_list(key, &name_error);
    if (!servers.empty())
    {
        // All servers in the array must be monitored by the given monitor.
        for (auto elem : servers)
        {
            MonitorServer* mon_serv = get_monitored_server(elem);
            if (mon_serv)
            {
                monitored_array.push_back(mon_serv);
            }
            else
            {
                MXS_ERROR("Server '%s' is not monitored by monitor '%s'.", elem->name(), name());
                *error_out = true;
            }
        }

        if (monitored_array.size() < servers.size())
        {
            monitored_array.clear();
        }
    }
    else
    {
        MXS_ERROR("Serverlist setting '%s' contains invalid server name '%s'.",
                  key.c_str(), name_error.c_str());
        *error_out = true;
    }

    return monitored_array;
}

bool Monitor::set_disk_space_threshold(const string& dst_setting)
{
    mxb_assert(state() == MONITOR_STATE_STOPPED);
    SERVER::DiskSpaceLimits new_dst;
    bool rv = config_parse_disk_space_threshold(&new_dst, dst_setting.c_str());
    if (rv)
    {
        m_settings.disk_space_limits = new_dst;
    }
    return rv;
}

bool Monitor::set_server_status(SERVER* srv, int bit, string* errmsg_out)
{
    MonitorServer* msrv = get_monitored_server(srv);
    mxb_assert(msrv);

    if (!msrv)
    {
        MXS_ERROR("Monitor %s requested to set status of server %s that it does not monitor.",
                  name(), srv->address);
        return false;
    }

    bool written = false;

    if (state() == MONITOR_STATE_RUNNING)
    {
        /* This server is monitored, in which case modifying any other status bit than Maintenance is
         * disallowed. */
        if (bit & ~(SERVER_MAINT | SERVER_DRAINING))
        {
            MXS_ERROR(ERR_CANNOT_MODIFY);
            if (errmsg_out)
            {
                *errmsg_out = ERR_CANNOT_MODIFY;
            }
        }
        else
        {
            /* Maintenance and being-drained are set/cleared using a special variable which the
             * monitor reads when starting the next update cycle. */

            int request;
            if (bit & SERVER_MAINT)
            {
                request = MonitorServer::MAINT_ON;
            }
            else
            {
                mxb_assert(bit & SERVER_DRAINING);
                request = MonitorServer::BEING_DRAINED_ON;
            }

            int previous_request = atomic_exchange_int(&msrv->status_request, request);
            written = true;
            // Warn if the previous request hasn't been read.
            if (previous_request != MonitorServer::NO_CHANGE)
            {
                MXS_WARNING(WRN_REQUEST_OVERWRITTEN);
            }
            // Also set a flag so the next loop happens sooner.
            m_status_change_pending.store(true, std::memory_order_release);
        }
    }
    else
    {
        /* The monitor is not running, the bit can be set directly */
        srv->set_status(bit);
        written = true;
    }

    return written;
}

bool Monitor::clear_server_status(SERVER* srv, int bit, string* errmsg_out)
{
    MonitorServer* msrv = get_monitored_server(srv);
    mxb_assert(msrv);

    if (!msrv)
    {
        MXS_ERROR("Monitor %s requested to clear status of server %s that it does not monitor.",
                  name(), srv->address);
        return false;
    }

    bool written = false;

    if (state() == MONITOR_STATE_RUNNING)
    {
        if (bit & ~(SERVER_MAINT | SERVER_DRAINING))
        {
            MXS_ERROR(ERR_CANNOT_MODIFY);
            if (errmsg_out)
            {
                *errmsg_out = ERR_CANNOT_MODIFY;
            }
        }
        else
        {
            int request;
            if (bit & SERVER_MAINT)
            {
                request = MonitorServer::MAINT_OFF;
            }
            else
            {
                mxb_assert(bit & SERVER_DRAINING);
                request = MonitorServer::BEING_DRAINED_OFF;
            }

            int previous_request = atomic_exchange_int(&msrv->status_request, request);
            written = true;
            // Warn if the previous request hasn't been read.
            if (previous_request != MonitorServer::NO_CHANGE)
            {
                MXS_WARNING(WRN_REQUEST_OVERWRITTEN);
            }
            // Also set a flag so the next loop happens sooner.
            m_status_change_pending.store(true, std::memory_order_release);
        }
    }
    else
    {
        /* The monitor is not running, the bit can be cleared directly */
        srv->clear_status(bit);
        written = true;
    }

    return written;
}

void Monitor::populate_services()
{
    mxb_assert(state() == MONITOR_STATE_STOPPED);

    for (MonitorServer* pMs : m_servers)
    {
        service_add_server(this, pMs->server);
    }
}

void Monitor::deactivate()
{
    if (state() == MONITOR_STATE_RUNNING)
    {
        stop();
    }
    remove_all_servers();
}

bool Monitor::check_disk_space_this_tick()
{
    bool should_update_disk_space = false;
    auto check_interval = m_settings.disk_space_check_interval;

    if ((check_interval.secs() > 0) && m_disk_space_checked.split() > check_interval)
    {
        should_update_disk_space = true;
        // Whether or not disk space check succeeds, reset the timer. This way, disk space is always
        // checked during the same tick for all servers.
        m_disk_space_checked.restart();
    }
    return should_update_disk_space;
}

bool Monitor::server_status_request_waiting() const
{
    return m_status_change_pending.load(std::memory_order_acquire);
}

MonitorWorker::MonitorWorker(const string& name, const string& module)
    : Monitor(name, module)
    , m_thread_running(false)
    , m_shutdown(0)
    , m_checked(false)
    , m_loop_called(get_time_ms())
{
}

MonitorWorker::~MonitorWorker()
{
}

monitor_state_t MonitorWorker::state() const
{
    bool running = (Worker::state() != Worker::STOPPED);

    return running ? MONITOR_STATE_RUNNING : MONITOR_STATE_STOPPED;
}

void MonitorWorker::do_stop()
{
    // This should only be called by monitor_stop(). NULL worker is allowed since the main worker may
    // not exist during program start/stop.
    mxb_assert(mxs_rworker_get_current() == NULL
               || mxs_rworker_get_current() == mxs_rworker_get(MXS_RWORKER_MAIN));
    mxb_assert(Worker::state() != Worker::STOPPED);
    mxb_assert(state() != MONITOR_STATE_STOPPED);
    mxb_assert(m_thread_running.load() == true);

    Worker::shutdown();
    Worker::join();
    m_thread_running.store(false, std::memory_order_release);
}

void MonitorWorker::diagnostics(DCB* pDcb) const
{
}

json_t* MonitorWorker::diagnostics_json() const
{
    return json_object();
}

bool MonitorWorker::start()
{
    // This should only be called by monitor_start(). NULL worker is allowed since the main worker may
    // not exist during program start/stop.
    mxb_assert(mxs_rworker_get_current() == NULL
               || mxs_rworker_get_current() == mxs_rworker_get(MXS_RWORKER_MAIN));
    mxb_assert(Worker::state() == Worker::STOPPED);
    mxb_assert(state() == MONITOR_STATE_STOPPED);
    mxb_assert(m_thread_running.load() == false);

    if (journal_is_stale())
    {
        MXS_WARNING("Removing stale journal file for monitor '%s'.", name());
        remove_server_journal();
    }

    if (!m_checked)
    {
        if (!has_sufficient_permissions())
        {
            MXS_ERROR("Failed to start monitor. See earlier errors for more information.");
        }
        else
        {
            m_checked = true;
        }
    }

    bool started = false;
    if (m_checked)
    {
        m_loop_called = get_time_ms() - settings().interval;    // Next tick should happen immediately.
        if (!Worker::start())
        {
            MXS_ERROR("Failed to start worker for monitor '%s'.", name());
        }
        else
        {
            // Ok, so the thread started. Let's wait until we can be certain the
            // state has been updated.
            m_semaphore.wait();

            started = m_thread_running.load(std::memory_order_acquire);
            if (!started)
            {
                // Ok, so the initialization failed and the thread will exit.
                // We need to wait on it so that the thread resources will not leak.
                Worker::join();
            }
        }
    }
    return started;
}

// static
int64_t MonitorWorker::get_time_ms()
{
    timespec t;

    MXB_AT_DEBUG(int rv = ) clock_gettime(CLOCK_MONOTONIC_COARSE, &t);
    mxb_assert(rv == 0);

    return t.tv_sec * 1000 + (t.tv_nsec / 1000000);
}

bool MonitorServer::can_update_disk_space_status() const
{
    return ok_to_check_disk_space && (!monitor_limits.empty() || server->have_disk_space_limits());
}

void MonitorServer::update_disk_space_status()
{
    auto pMs = this;    // TODO: Clean
    std::map<std::string, disk::SizesAndName> info;

    int rv = disk::get_info_by_path(pMs->con, &info);

    if (rv == 0)
    {
        // Server-specific setting takes precedence.
        auto dst = pMs->server->get_disk_space_limits();
        if (dst.empty())
        {
            dst = monitor_limits;
        }

        bool disk_space_exhausted = false;
        int32_t star_max_percentage = -1;
        std::set<std::string> checked_paths;

        for (const auto& dst_item : dst)
        {
            string path = dst_item.first;
            int32_t max_percentage = dst_item.second;

            if (path == "*")
            {
                star_max_percentage = max_percentage;
            }
            else
            {
                auto j = info.find(path);

                if (j != info.end())
                {
                    const disk::SizesAndName& san = j->second;

                    disk_space_exhausted = check_disk_space_exhausted(pMs, path, san, max_percentage);
                    checked_paths.insert(path);
                }
                else
                {
                    MXS_WARNING("Disk space threshold specified for %s even though server %s at %s"
                                "does not have that.",
                                path.c_str(),
                                pMs->server->name(),
                                pMs->server->address);
                }
            }
        }

        if (star_max_percentage != -1)
        {
            for (auto j = info.begin(); j != info.end(); ++j)
            {
                string path = j->first;

                if (checked_paths.find(path) == checked_paths.end())
                {
                    const disk::SizesAndName& san = j->second;

                    disk_space_exhausted = check_disk_space_exhausted(pMs, path, san, star_max_percentage);
                }
            }
        }

        if (disk_space_exhausted)
        {
            pMs->pending_status |= SERVER_DISK_SPACE_EXHAUSTED;
        }
        else
        {
            pMs->pending_status &= ~SERVER_DISK_SPACE_EXHAUSTED;
        }
    }
    else
    {
        SERVER* pServer = pMs->server;

        if (mysql_errno(pMs->con) == ER_UNKNOWN_TABLE)
        {
            // Disable disk space checking for this server.
            pMs->ok_to_check_disk_space = false;

            MXS_ERROR("Disk space cannot be checked for %s at %s, because either the "
                      "version (%s) is too old, or the DISKS information schema plugin "
                      "has not been installed. Disk space checking has been disabled.",
                      pServer->name(),
                      pServer->address,
                      pServer->version_string().c_str());
        }
        else
        {
            MXS_ERROR("Checking the disk space for %s at %s failed due to: (%d) %s",
                      pServer->name(),
                      pServer->address,
                      mysql_errno(pMs->con),
                      mysql_error(pMs->con));
        }
    }
}

bool MonitorWorker::configure(const MXS_CONFIG_PARAMETER* pParams)
{
    return Monitor::configure(pParams);
}

bool MonitorWorker::has_sufficient_permissions()
{
    return true;
}

void MonitorWorker::flush_server_status()
{
    for (MonitorServer* pMs : m_servers)
    {
        if (!pMs->server->is_in_maint())
        {
            pMs->server->status = pMs->pending_status;
        }
    }
}

void MonitorWorkerSimple::pre_loop()
{
    m_master = nullptr;
    load_server_journal(&m_master);
    // Add another overridable function for derived classes (e.g. pre_loop_monsimple) if required.
}

void MonitorWorkerSimple::post_loop()
{
}

void MonitorWorkerSimple::pre_tick()
{
}

void MonitorWorkerSimple::post_tick()
{
}

void MonitorWorkerSimple::tick()
{
    check_maintenance_requests();
    pre_tick();

    const bool should_update_disk_space = check_disk_space_this_tick();

    for (MonitorServer* pMs : m_servers)
    {
        if (!pMs->server->is_in_maint())
        {
            pMs->mon_prev_status = pMs->server->status;
            pMs->pending_status = pMs->server->status;

            mxs_connect_result_t rval = pMs->ping_or_connect(settings().conn_settings);

            if (connection_is_ok(rval))
            {
                pMs->clear_pending_status(SERVER_AUTH_ERROR);
                pMs->set_pending_status(SERVER_RUNNING);

                if (should_update_disk_space && pMs->can_update_disk_space_status())
                {
                    pMs->update_disk_space_status();
                }

                update_server_status(pMs);
            }
            else
            {
                /**
                 * TODO: Move the bits that do not represent a state out of
                 * the server state bits. This would allow clearing the state by
                 * zeroing it out.
                 */
                const uint64_t bits_to_clear = ~SERVER_WAS_MASTER;

                pMs->clear_pending_status(bits_to_clear);

                if (mysql_errno(pMs->con) == ER_ACCESS_DENIED_ERROR)
                {
                    pMs->set_pending_status(SERVER_AUTH_ERROR);
                }
                else
                {
                    pMs->clear_pending_status(SERVER_AUTH_ERROR);
                }

                if (pMs->status_changed() && pMs->should_print_fail_status())
                {
                    pMs->log_connect_error(rval);
                }
            }

#if defined (SS_DEBUG)
            if (pMs->status_changed() || pMs->should_print_fail_status())
            {
                // The current status is still in pMs->pending_status.
                MXS_DEBUG("Backend server [%s]:%d state : %s",
                          pMs->server->address, pMs->server->port,
                          SERVER::status_to_string(pMs->pending_status).c_str());
            }
#endif

            if (pMs->server->is_down())
            {
                pMs->mon_err_count += 1;
            }
            else
            {
                pMs->mon_err_count = 0;
            }
        }
    }

    post_tick();

    flush_server_status();
    process_state_changes();
    hangup_failed_servers();
    store_server_journal(m_master);
}

void MonitorWorker::pre_loop()
{
}

void MonitorWorker::post_loop()
{
}

void MonitorWorker::process_state_changes()
{
    detect_handle_state_changes();
}

bool MonitorWorker::pre_run()
{
    bool rv = false;

    if (mysql_thread_init() == 0)
    {
        rv = true;
        // Write and post the semaphore to signal the admin thread that the start is succeeding.
        m_thread_running.store(true, std::memory_order_release);
        m_semaphore.post();

        pre_loop();
        delayed_call(1, &MonitorWorker::call_run_one_tick, this);
    }
    else
    {
        MXS_ERROR("mysql_thread_init() failed for %s. The monitor cannot start.", name());
        m_semaphore.post();
    }

    return rv;
}

void MonitorWorker::post_run()
{
    post_loop();

    mysql_thread_end();
}

bool MonitorWorker::call_run_one_tick(Worker::Call::action_t action)
{
    /** This is both the minimum sleep between two ticks and also the maximum time between early
     *  wakeup checks. */
    const int base_interval_ms = 100;
    if (action == Worker::Call::EXECUTE)
    {
        int64_t now = get_time_ms();
        // Enough time has passed,
        if ((now - m_loop_called > settings().interval)
            // or a server status change request is waiting,
            || server_status_request_waiting()
            // or a monitor-specific condition is met.
            || immediate_tick_required())
        {
            m_loop_called = now;
            run_one_tick();
            now = get_time_ms();
        }

        int64_t ms_to_next_call = settings().interval - (now - m_loop_called);
        // ms_to_next_call will be negative, if the run_one_tick() call took
        // longer than one monitor interval.
        int64_t delay = ((ms_to_next_call <= 0) || (ms_to_next_call >= base_interval_ms)) ?
            base_interval_ms : ms_to_next_call;

        delayed_call(delay, &MonitorWorker::call_run_one_tick, this);
    }
    return false;
}

void MonitorWorker::run_one_tick()
{
    tick();
    m_ticks.fetch_add(1, std::memory_order_acq_rel);
}

bool MonitorWorker::immediate_tick_required() const
{
    return false;
}

MonitorServer::MonitorServer(SERVER* server, const SERVER::DiskSpaceLimits& monitor_limits)
    : server(server)
    , monitor_limits(monitor_limits)
{
}

MonitorServer::~MonitorServer()
{
    if (con)
    {
        mysql_close(con);
    }
}
}
