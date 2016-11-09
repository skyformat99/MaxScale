/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file debugcmd.c  - The debug CLI command line interpreter
 *
 * The command interpreter for the dbug user interface. The command
 * structure is such that there are a numerb of commands, notably
 * show and a set of subcommands, the things to show in this case.
 *
 * Each subcommand has a handler function defined for it that is passeed
 * the DCB to use to print the output of the commands and up to 3 arguments
 * as numeric values.
 *
 * There are two "built in" commands, the help command and the quit
 * command.
 *
 * @verbatim
 * Revision History
 *
 * Date         Who                     Description
 * 20/06/13     Mark Riddoch            Initial implementation
 * 17/07/13     Mark Riddoch            Additional commands
 * 09/08/13     Massimiliano Pinto      Added enable/disable commands (now only for log)
 * 20/05/14     Mark Riddoch            Added ability to give server and service names rather
 *                                      than simply addresses
 * 23/05/14     Mark Riddoch            Added support for developer and user modes
 * 29/05/14     Mark Riddoch            Add Filter support
 * 16/10/14     Mark Riddoch            Add show eventq
 * 05/03/15     Massimiliano Pinto      Added enable/disable feedback
 * 27/05/15     Martin Brampton         Add show persistent [server]
 * 06/11/15     Martin Brampton         Add show buffers (conditional compilation)
 * 23/05/16     Massimiliano Pinto      'add user' and 'remove user'
 *                                      no longer accept password parameter
 * 27/06/16     Martin Brampton         Modify to work with list manager sessions
 *
 * @endverbatim
 */
#include <my_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <maxscale/service.h>
#include <maxscale/session.h>
#include <maxscale/router.h>
#include <maxscale/filter.h>
#include <maxscale/modules.h>
#include <maxscale/atomic.h>
#include <maxscale/server.h>
#include <maxscale/spinlock.h>
#include <maxscale/buffer.h>
#include <maxscale/dcb.h>
#include <maxscale/poll.h>
#include <maxscale/users.h>
#include <maxscale/config.h>
#include <telnetd.h>
#include <maxscale/adminusers.h>
#include <maxscale/monitor.h>
#include <debugcli.h>
#include <maxscale/housekeeper.h>
#include <maxscale/listmanager.h>
#include <maxscale/maxscale.h>

#include <maxscale/log_manager.h>
#include <sys/syslog.h>

#define MAXARGS 12

#define ARG_TYPE_NONE           0
#define ARG_TYPE_ADDRESS        1
#define ARG_TYPE_STRING         2
#define ARG_TYPE_SERVICE        3
#define ARG_TYPE_SERVER         4
#define ARG_TYPE_DBUSERS        5
#define ARG_TYPE_SESSION        6
#define ARG_TYPE_DCB            7
#define ARG_TYPE_MONITOR        8
#define ARG_TYPE_FILTER         9
#define ARG_TYPE_NUMERIC        10

extern LIST_CONFIG SESSIONlist;

/**
 * The subcommand structure
 *
 * These are the options that may be passed to a command
 */
struct subcommand
{
    char    *arg1;
    int     argc_min;
    int     argc_max;
    void    (*fn)();
    char    *help;
    char    *devhelp;
    int     arg_types[MAXARGS];
};

#define EMPTY_OPTION

static void telnetdShowUsers(DCB *);
static void show_log_throttling(DCB *);

/**
 * The subcommands of the show command
 */
struct subcommand showoptions[] =
{
#if defined(BUFFER_TRACE)
    {
        "buffers",    0, dprintAllBuffers,
        "Show all buffers with backtrace",
        "Show all buffers with backtrace",
        {0, 0, 0}
    },
#endif
    {
        "dcblist", 0, 0, dprintDCBList,
        "Show DCB statistics",
        "Show statistics for the list of all DCBs(descriptor control blocks)",
        {0}
    },
    {
        "dcbs", 0, 0, dprintAllDCBs,
        "Show all DCBs",
        "Show all descriptor control blocks (network connections)",
        {0}
    },
    {
        "dcb", 1, 1, dprintDCB,
        "Show a DCB",
        "Show a single descriptor control block e.g. show dcb 0x493340",
        {ARG_TYPE_DCB, 0, 0}
    },
    {
        "dbusers", 1, 1, dcb_usersPrint,
        "Show user statistics",
        "Show statistics and user names for a service's user table.\n"
        "\t\tExample : show dbusers <ptr of 'User's data' from services list>|<service name>",
        {ARG_TYPE_DBUSERS, 0, 0}
    },
    {
        "epoll", 0, 0, dprintPollStats,
        "Show the poll statistics",
        "Show the epoll polling system statistics",
        {0, 0, 0}
    },
    {
        "eventq", 0, 0, dShowEventQ,
        "Show event queue",
        "Show the queue of events waiting to be processed",
        {0, 0, 0}
    },
    {
        "eventstats", 0, 0, dShowEventStats,
        "Show event queue statistics",
        "Show event queue statistics",
        {0, 0, 0}
    },
    {
        "feedbackreport", 0, 0, moduleShowFeedbackReport,
        "Show feedback report",
        "Show the report of MaxScale loaded modules, suitable for Notification Service",
        {0, 0, 0}
    },
    {
        "filter", 1, 1, dprintFilter,
        "Show filter details",
        "Show details of a filter, the parameter is filter name",
        {ARG_TYPE_FILTER, 0, 0}
    },
    {
        "filters", 0, 0, dprintAllFilters,
        "Show all filters",
        "Show all filters that were read from the configuration file",
        {0, 0, 0}
    },
    {
        "log_throttling", 0, 0, show_log_throttling,
        "Show log throttling setting",
        "Show the current log throttling setting (count, window (ms), suppression (ms))",
        {0, 0, 0}
    },
    {
        "modules", 0, 0, dprintAllModules,
        "Show loaded modules",
        "Show all currently loaded modules",
        {0, 0, 0}
    },
    {
        "monitor", 1, 1, monitorShow,
        "Show monitor details",
        "Show details about a specific monitor, the parameter is monitor name",
        {ARG_TYPE_MONITOR, 0, 0}
    },
    {
        "monitors", 0, 0, monitorShowAll,
        "Show all monitors",
        "Show all the monitors",
        {0, 0, 0}
    },
    {
        "persistent", 1, 1, dprintPersistentDCBs,
        "Show persistent connection pool",
        "Show persistent pool for a server, e.g. show persistent dbnode1. ",
        {ARG_TYPE_SERVER, 0, 0}
    },
    {
        "server", 1, 1, dprintServer,
        "Show server details",
        "Show details for a server, e.g. show server dbnode1",
        {ARG_TYPE_SERVER, 0, 0}
    },
    {
        "servers", 0, 0, dprintAllServers,
        "Show all servers",
        "Show all configured servers",
        {0, 0, 0}
    },
    {
        "serversjson", 0, 0, dprintAllServersJson,
        "Show all servers in JSON",
        "Show all configured servers in JSON format",
        {0, 0, 0}
    },
    {
        "services", 0, 0, dprintAllServices,
        "Show all service",
        "Show all configured services in MaxScale",
        {0, 0, 0}
    },
    {
        "service", 1, 1, dprintService,
        "Show service details",
        "Show a single service in MaxScale, the parameter is the service name",
        {ARG_TYPE_SERVICE, 0, 0}
    },
    {
        "session", 1, 1, dprintSession,
        "Show session details",
        "Show a single session in MaxScale, e.g. show session 0x284830",
        {ARG_TYPE_SESSION, 0, 0}
    },
    {
        "sessionlist", 0, 0, dprintSessionList,
        "Show session list statistics",
        "Show statistics for the list of all sessions",
        {0, 0, 0}
    },
    {
        "sessions", 0, 0, dprintAllSessions,
        "Show all sessions",
        "Show all active sessions in MaxScale",
        {0, 0, 0}
    },
    {
        "tasks", 0, 0, hkshow_tasks,
        "Show housekeeper tasks",
        "Show all active housekeeper tasks in MaxScale",
        {0, 0, 0}
    },
    {
        "threads", 0, 0, dShowThreads,
        "Show workter thread status",
        "Show the status of the worker threads in MaxScale",
        {0, 0, 0}
    },
    {
        "users", 0, 0, telnetdShowUsers,
        "Show enabled Linux accounts",
        "Show all maxadmin enabled Linux accounts and created maxadmin users",
        {0, 0, 0}
    },
    { EMPTY_OPTION}
};

/**
 * The subcommands of the list command
 */
struct subcommand listoptions[] =
{
    {
        "clients", 0, 0, dListClients,
        "List all clients",
        "List all the client connections to MaxScale",
        {0, 0, 0}
    },
    {
        "dcbs", 0, 0, dListDCBs,
        "List all DCBs",
        "List all the DCBs active within MaxScale",
        {0, 0, 0}
    },
    {
        "filters", 0, 0, dListFilters,
        "List all filters",
        "List all the filters defined within MaxScale",
        {0, 0, 0}
    },
    {
        "listeners", 0, 0, dListListeners,
        "List all listeners",
        "List all the listeners defined within MaxScale",
        {0, 0, 0}
    },
    {
        "modules", 0, 0, dprintAllModules,
        "List all currently loaded modules",
        "List all currently loaded modules",
        {0, 0, 0}
    },
    {
        "monitors", 0, 0, monitorList,
        "List all monitors",
        "List all monitors",
        {0, 0, 0}
    },
    {
        "services", 0, 0, dListServices,
        "List all the services",
        "List all the services defined within MaxScale",
        {0, 0, 0}
    },
    {
        "servers", 0, 0, dListServers,
        "List all servers",
        "List all the servers defined within MaxScale",
        {0, 0, 0}
    },
    {
        "sessions", 0, 0, dListSessions,
        "List all sessions",
        "List all the active sessions within MaxScale",
        {0, 0, 0}
    },
    {
        "threads", 0, 0, dShowThreads,
        "List polling threads",
        "List the status of the polling threads in MaxScale",
        {0, 0, 0}
    },
    { EMPTY_OPTION}
};

static void shutdown_server()
{
    maxscale_shutdown();
}

static void shutdown_service(DCB *dcb, SERVICE *service);
static void shutdown_monitor(DCB *dcb, MONITOR *monitor);

/**
 * The subcommands of the shutdown command
 */
struct subcommand shutdownoptions[] =
{
    {
        "maxscale",
        0, 0,
        shutdown_server,
        "Shutdown MaxScale",
        "Initiate a controlled shutdown of MaxScale",
        {0, 0, 0}
    },
    {
        "monitor",
        1, 1,
        shutdown_monitor,
        "Shutdown a monitor",
        "E.g. shutdown monitor db-cluster-monitor",
        {ARG_TYPE_MONITOR, 0, 0}
    },
    {
        "service",
        1, 1,
        shutdown_service,
        "Stop a service",
        "E.g. shutdown service \"Sales Database\"",
        {ARG_TYPE_SERVICE, 0, 0}
    },
    {
        EMPTY_OPTION
    }
};

static void sync_logs(DCB *dcb)
{
    if (mxs_log_flush_sync() == 0)
    {
        dcb_printf(dcb, "Logs flushed to disk\n");
    }
    else
    {
        dcb_printf(dcb, "Failed to flush logs to disk. Read the error log for "
                   "more details.\n");
    }
}

struct subcommand syncoptions[] =
{
    {
        "logs",
        0, 0,
        sync_logs,
        "Flush log files to disk",
        "Flush log files to disk",
        {0, 0, 0}
    },
    {
        EMPTY_OPTION
    }
};

static void restart_service(DCB *dcb, SERVICE *service);
static void restart_monitor(DCB *dcb, MONITOR *monitor);
/**
 * The subcommands of the restart command
 */
struct subcommand restartoptions[] =
{
    {
        "monitor", 1, 1, restart_monitor,
        "Restart a monitor",
        "E.g. restart monitor db-cluster-monitor",
        {ARG_TYPE_MONITOR, 0, 0}
    },
    {
        "service", 1, 1, restart_service,
        "Restart a service",
        "E.g. restart service \"Sales Database\"",
        {ARG_TYPE_SERVICE, 0, 0}
    },
    { EMPTY_OPTION }
};

static void set_server(DCB *dcb, SERVER *server, char *bit);
static void set_pollsleep(DCB *dcb, int);
static void set_nbpoll(DCB *dcb, int);
static void set_log_throttling(DCB *dcb, int count, int window_ms, int suppress_ms);
/**
 * The subcommands of the set command
 */
struct subcommand setoptions[] =
{
    {
        "server", 2, 2, set_server,
        "Set the status of a server",
        "Set the status of a server. E.g. set server dbnode4 master",
        {ARG_TYPE_SERVER, ARG_TYPE_STRING, 0}
    },
    {
        "pollsleep", 1, 1, set_pollsleep,
        "Set poll sleep period",
        "Set the maximum poll sleep period in milliseconds",
        {ARG_TYPE_NUMERIC, 0, 0}
    },
    {
        "nbpolls", 1, 1, set_nbpoll,
        "Set non-blocking polls",
        "Set the number of non-blocking polls",
        {ARG_TYPE_NUMERIC, 0, 0}
    },
    {
        "log_throttling", 3, 3, set_log_throttling,
        "Set log throttling",
        "Set the log throttling configuration",
        {ARG_TYPE_NUMERIC, ARG_TYPE_NUMERIC, ARG_TYPE_NUMERIC}
    },
    { EMPTY_OPTION }
};

static void clear_server(DCB *dcb, SERVER *server, char *bit);
/**
 * The subcommands of the clear command
 */
struct subcommand clearoptions[] =
{
    {
        "server", 2, 2, clear_server,
        "Clear server status",
        "Clear the status of a server. E.g. clear server dbnode2 master",
        {ARG_TYPE_SERVER, ARG_TYPE_STRING, 0}
    },
    { EMPTY_OPTION }
};

static void reload_dbusers(DCB *dcb, SERVICE *service);
static void reload_config(DCB *dcb);

/**
 * The subcommands of the reload command
 */
struct subcommand reloadoptions[] =
{
    {
        "config", 0, 0, reload_config,
        "Reload the configuration",
        "Reload the configuration data for MaxScale",
        {0, 0, 0}
    },
    {
        "dbusers", 1, 1, reload_dbusers,
        "Reload users table",
        "Reload the users for a service. E.g. reload dbusers \"splitter service\"",
        {ARG_TYPE_SERVICE, 0, 0}
    },
    { EMPTY_OPTION }
};

static void enable_log_action(DCB *, char *);
static void disable_log_action(DCB *, char *);
static void enable_log_priority(DCB *, char *);
static void disable_log_priority(DCB *, char *);
static void enable_sess_log_action(DCB *dcb, char *arg1, char *arg2);
static void disable_sess_log_action(DCB *dcb, char *arg1, char *arg2);
static void enable_sess_log_priority(DCB *dcb, char *arg1, char *arg2);
static void disable_sess_log_priority(DCB *dcb, char *arg1, char *arg2);
static void enable_monitor_replication_heartbeat(DCB *dcb, MONITOR *monitor);
static void disable_monitor_replication_heartbeat(DCB *dcb, MONITOR *monitor);
static void enable_service_root(DCB *dcb, SERVICE *service);
static void disable_service_root(DCB *dcb, SERVICE *service);
static void enable_feedback_action();
static void disable_feedback_action();
static void enable_syslog();
static void disable_syslog();
static void enable_maxlog();
static void disable_maxlog();
static void enable_account(DCB *, char *user);
static void disable_account(DCB *, char *user);

/**
 *  * The subcommands of the enable command
 *   */
struct subcommand enableoptions[] =
{
    {
        "heartbeat",
        1, 1,
        enable_monitor_replication_heartbeat,
        "Enable monitor replication heartbeat",
        "Enable the monitor replication heartbeat, the parameter is the monitor name",
        {ARG_TYPE_MONITOR, 0, 0}
    },
    {
        "log",
        1, 1,
        enable_log_action,
        "[deprecated] Enable a logging level",
        "Options 'trace' | 'error' | 'message'. E.g. 'enable log message'.",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "log-priority",
        1, 1,
        enable_log_priority,
        "Enable a logging priority",
        "Enable a logging priority for MaxScale, parameters must be one of "
        "'err', 'warning', 'notice', 'info' or 'debug'. "
        "E.g.: 'enable log-priority info'.",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "sessionlog",
        2, 2,
        enable_sess_log_action,
        "[deprecated] Enable a logging level for a single session",
        "Usage: enable sessionlog [trace | error | "
        "message | debug] <session id>\t E.g. enable sessionlog message 123.",
        {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
    },
    {
        "sessionlog-priority",
        2, 2,
        enable_sess_log_priority,
        "Enable a logging priority for a session",
        "Usage: enable sessionlog-priority [err | warning | notice | info | debug] <session id>"
        "message | debug] <session id>\t E.g. enable sessionlog-priority info 123.",
        {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
    },
    {
        "root",
        1, 1,
        enable_service_root,
        "Enable root user access",
        "Enable root access to a service, pass a service name to enable root access",
        {ARG_TYPE_SERVICE, 0, 0}
    },
    {
        "feedback",
        0, 0,
        enable_feedback_action,
        "Enable MaxScale feedback",
        "Enable MaxScale modules list sending via http to notification service",
        {0, 0, 0}
    },
    {
        "syslog",
        0, 0,
        enable_syslog,
        "Enable syslog",
        "Enable syslog logging",
        {0, 0, 0}
    },
    {
        "maxlog",
        0, 0,
        enable_maxlog,
        "Enable MaxScale logging",
        "Enable MaxScale logging",
        {0, 0, 0}
    },
    {
        "account",
        1, 1,
        enable_account,
        "Activate a Linux user",
        "Enable maxadmin usage for a Linux user. E.g.:\n"
        "                 MaxScale> enable account alice",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        EMPTY_OPTION
    }
};



/**
 *  * The subcommands of the disable command
 *   */
struct subcommand disableoptions[] =
{
    {
        "heartbeat",
        1, 1,
        disable_monitor_replication_heartbeat,
        "Disable replication heartbeat",
        "Disable the monitor replication heartbeat",
        {ARG_TYPE_MONITOR, 0, 0}
    },
    {
        "log",
        1, 1,
        disable_log_action,
        "[deprecated] Disable log for MaxScale",
        "Options: 'debug' | 'trace' | 'error' | 'message'."
        "E.g. 'disable log debug'",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "log-priority",
        1, 1,
        disable_log_priority,
        "Disable a logging priority",
        "Options 'err' | 'warning' | 'notice' | 'info' | 'debug'. "
        "E.g.: 'disable log-priority info'",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "sessionlog",
        2, 2,
        disable_sess_log_action,
        "[deprecated] Disable log options",
        "Disable Log options for a single session. Usage: disable sessionlog [trace | error | "
        "message | debug] <session id>\t E.g. disable sessionlog message 123",
        {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
    },
    {
        "sessionlog-priority",
        2, 2,
        disable_sess_log_priority,
        "Disable a logging priority for a particular session",
        "Usage: disable sessionlog-priority [err | warning | notice | info | debug] <session id>"
        "message | debug] <session id>\t E.g. enable sessionlog-priority info 123",
        {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
    },
    {
        "root",
        1, 1,
        disable_service_root,
        "Disable root access",
        "Disable root access to a service",
        {ARG_TYPE_SERVICE, 0, 0}
    },
    {
        "feedback",
        0, 0,
        disable_feedback_action,
        "Disable feedback",
        "Disable MaxScale modules list sending via http to notification service",
        {0, 0, 0}
    },
    {
        "syslog",
        0, 0,
        disable_syslog,
        "Disable syslog",
        "Disable syslog logging",
        {0, 0, 0}
    },
    {
        "maxlog",
        0, 0,
        disable_maxlog,
        "Disable MaxScale logging",
        "Disable MaxScale logging",
        {0, 0, 0}
    },
    {
        "account",
        1, 1,
        disable_account,
        "Disable Linux user",
        "Disable maxadmin usage for Linux user. E.g.:\n"
        "                 MaxScale> disable account alice",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        EMPTY_OPTION
    }
};

#if defined(FAKE_CODE)

static void fail_backendfd(void);
static void fail_clientfd(void);
static void fail_accept(DCB* dcb, char* arg1, char* arg2);
/**
 *  * The subcommands of the fail command
 *   */
struct subcommand failoptions[] =
{
    {
        "backendfd",
        0,
        fail_backendfd,
        "Fail backend socket for next operation.",
        "Fail backend socket for next operation.",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "clientfd",
        0,
        fail_clientfd,
        "Fail client socket for next operation.",
        "Fail client socket for next operation.",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "accept",
        2,
        fail_accept,
        "Fail to accept next client connection.",
        "Fail to accept next client connection.",
        {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
    },
    {
        NULL,
        0,
        NULL,
        NULL,
        NULL,
        {0, 0, 0}
    }
};
#endif /* FAKE_CODE */

static void telnetdAddUser(DCB *, char *user, char *password);

static void cmd_AddServer(DCB *dcb, void *a, void *b)
{
    SERVER *server = (SERVER*)a;
    char *name = (char*)b;

    SERVICE *service = service_find(name);
    MONITOR *monitor = monitor_find(name);

    if (service || monitor)
    {
        ss_dassert(service == NULL || monitor == NULL);

        if (service)
        {
            serviceAddBackend(service, server);
        }
        else if (monitor)
        {
            monitorAddServer(monitor, server);
        }

        const char *target = service ? "service" : "monitor";

        MXS_NOTICE("Added server '%s' to %s '%s'", server->unique_name, target, name);
        dcb_printf(dcb, "Added server '%s' to %s '%s'\n", server->unique_name, target, name);
    }
    else
    {
        dcb_printf(dcb, "No service or monitor with the name '%s'\n", name);
    }
}

/**
 * The subcommands of the add command
 */
struct subcommand addoptions[] =
{
    {
        "user", 2, 2, telnetdAddUser,
        "Add account for maxadmin",
        "Add insecure account for using maxadmin over the network. E.g.:\n"
        "                 MaxScale> add user bob somepass",
        {ARG_TYPE_STRING, ARG_TYPE_STRING, 0}
    },
    {
        "server", 2, 2, cmd_AddServer,
        "Add a new server to a service",
        "Usage: add server SERVER TARGET\n"
        "The TARGET must be either a service or a monitor",
        {ARG_TYPE_SERVER, ARG_TYPE_STRING, 0}
    },
    { EMPTY_OPTION}
};


static void telnetdRemoveUser(DCB *, char *user, char *password);

static void cmd_RemoveServer(DCB *dcb, void *a, void *b)
{
    SERVER *server = (SERVER*)a;
    char *name = (char*)b;
    SERVICE *service = service_find(name);
    MONITOR *monitor = monitor_find(name);

    if (service || monitor)
    {
        ss_dassert(service == NULL || monitor == NULL);

        if (service)
        {
            serviceRemoveBackend(service, server);
        }
        else if (monitor)
        {
            monitorRemoveServer(monitor, server);
        }

        const char *target = service ? "service" : "monitor";
        MXS_NOTICE("Removed server '%s' from %s '%s'", server->unique_name, target, name);
        dcb_printf(dcb, "Removed server '%s' from %s '%s'\n", server->unique_name, target, name);
    }
    else
    {
        dcb_printf(dcb, "No service or monitor with the name '%s'\n", name);
    }
}

/**
 * The subcommands of the remove command
 */
struct subcommand removeoptions[] =
{
    {
        "user",
        2, 2,
        telnetdRemoveUser,
        "Remove account from maxadmin",
        "Remove account for using maxadmin over the network. E.g.:\n"
        "                 MaxAdmin> remove user bob somepass",
        {ARG_TYPE_STRING, ARG_TYPE_STRING}
    },
    {
        "server", 2, 2, cmd_RemoveServer,
        "Remove a server from a service or a monitor",
        "Usage: remove server SERVER TARGET\n"
        "The TARGET must be either a service or a monitor",
        {ARG_TYPE_SERVER, ARG_TYPE_STRING}
    },
    {
        EMPTY_OPTION
    }
};

/**
 * User command to flush a single logfile
 *
 * @param pdcb          The stream to write output to
 * @param logname       The name of the log
 */
static void
flushlog(DCB *pdcb, char *logname)
{
    bool unrecognized = false;
    bool deprecated = false;

    if (!strcasecmp(logname, "error"))
    {
        deprecated = true;
    }
    else if (!strcasecmp(logname, "message"))
    {
        deprecated = true;
    }
    else if (!strcasecmp(logname, "trace"))
    {
        deprecated = true;
    }
    else if (!strcasecmp(logname, "debug"))
    {
        deprecated = true;
    }
    else if (!strcasecmp(logname, "maxscale"))
    {
        ; // nop
    }
    else
    {
        unrecognized = true;
    }

    if (unrecognized)
    {
        dcb_printf(pdcb, "Unexpected logfile name '%s', expected: 'maxscale'.\n", logname);
    }
    else
    {
        mxs_log_rotate();

        if (deprecated)
        {
            dcb_printf(pdcb,
                       "'%s' is deprecated, currently there is only one log 'maxscale', "
                       "which was rotated.\n", logname);
        }
    }
}

/**
 * User command to flush all logfiles
 *
 * @param pdcb          The stream to write output to
 */
static void
flushlogs(DCB *pdcb)
{
    mxs_log_rotate();
}


/**
 * The subcommands of the flush command
 */
struct subcommand flushoptions[] =
{
    {
        "log",
        1, 1,
        flushlog,
        "Flush log files",
        "Flush the content of a log file, close that log, rename it and open a new log file",
        {ARG_TYPE_STRING, 0, 0}
    },
    {
        "logs",
        0, 0,
        flushlogs,
        "Flush log files",
        "Flush the content of all log files, close those logs, rename them and open a new log files",
        {0, 0, 0}
    },
    {
        EMPTY_OPTION
    }
};

/** This is used to prevent concurrent creation or removal of servers */
static SPINLOCK server_mod_lock = SPINLOCK_INIT;

static void createServer(DCB *dcb, char *name, char *address, char *port,
                         char *protocol, char *authenticator, char *authenticator_options)
{
    spinlock_acquire(&server_mod_lock);

    if (server_find_by_unique_name(name) == NULL)
    {
        if (protocol == NULL)
        {
            protocol = "MySQLBackend";
        }

        SERVER *server = server_alloc(address, protocol, atoi(port), authenticator,
                                      authenticator_options);

        if (server)
        {
            server_set_unique_name(server, name);
            // TODO: Serialize the server to disk
            dcb_printf(dcb, "Created server '%s'\n", name);
        }
        else
        {
            dcb_printf(dcb, "Failed to create new server, see log file for more details\n");
        }
    }
    else
    {
        dcb_printf(dcb, "Server '%s' already exists.\n", name);
    }

    spinlock_release(&server_mod_lock);
}

struct subcommand createoptions[] =
{
    {
        "server", 3, 6, createServer,
        "Create a new server",
        "Usage: create server NAME HOST PORT [PROTOCOL] [AUTHENTICATOR] [OPTIONS]\n"
        "Create a new server from the following parameters.\n"
        "NAME          Server name\n"
        "HOST          Server host address\n"
        "PORT          Server port\n"
        "PROTOCOL      Server protocol (default MySQLBackend)\n"
        "AUTHENTICATOR Authenticator module name (default MySQLAuth)\n"
        "OPTIONS       Options for the authenticator module\n\n"
        "The first three parameters are required, the others are optional.\n",
        {
            ARG_TYPE_STRING, ARG_TYPE_STRING, ARG_TYPE_STRING, ARG_TYPE_STRING,
            ARG_TYPE_STRING, ARG_TYPE_STRING
        }
    },
    {
        EMPTY_OPTION
    }
};

static void destroyServer(DCB *dcb, SERVER *server)
{
    dcb_printf(dcb, "Not yet implemented.\n");
}

struct subcommand destroyoptions[] =
{
    {
        "server", 1, 1, destroyServer,
        "Destroy a server",
        "Usage: destroy server NAME",
        {ARG_TYPE_STRING}
    },
    {
        EMPTY_OPTION
    }
};

static void alterServer(DCB *dcb, SERVER *server, char *key, char *value)
{
    bool unknown = false;

    if (strcmp(key, "address") == 0)
    {
        server_update_address(server, value);
    }
    else if (strcmp(key, "port") == 0)
    {
        server_update_port(server, atoi(value));
    }
    else if (strcmp(key, "monuser") == 0)
    {
        server_update_credentials(server, value, server->monpw);
    }
    else if (strcmp(key, "monpw") == 0)
    {
        server_update_credentials(server, server->monuser, value);
    }
    else if (server_is_ssl_parameter(key))
    {
        server_update_ssl(server, key, value);
    }
    else
    {
        unknown = true;
    }

    if (unknown)
    {
        dcb_printf(dcb, "Unknown parameter '%s'", key);
    }
}

struct subcommand alteroptions[] =
{
    {
        "server", 3, 3, alterServer,
        "Alter server parameters",
        "Usage: alter server NAME KEY VALUE\n"
        "This will alter an existing parameter of a server. The accepted values\n"
        "for KEY are: 'address', 'port', 'monuser', 'monpw'",
        {ARG_TYPE_SERVER, ARG_TYPE_STRING, ARG_TYPE_STRING}
    },
    {
        EMPTY_OPTION
    }
};

/**
 * The debug command table
 */
static struct
{
    char               *cmd;
    struct  subcommand *options;
} cmds[] =
{
    { "add",        addoptions },
    { "remove",     removeoptions },
    { "create",     createoptions },
    { "destroy",    destroyoptions },
    { "alter",      alteroptions },
    { "set",        setoptions },
    { "clear",      clearoptions },
    { "disable",    disableoptions },
    { "enable",     enableoptions },
#if defined(FAKE_CODE)
    { "fail",       failoptions },
#endif /* FAKE_CODE */
    { "flush",      flushoptions },
    { "list",       listoptions },
    { "reload",     reloadoptions },
    { "restart",    restartoptions },
    { "shutdown",   shutdownoptions },
    { "show",       showoptions },
    { "sync",   syncoptions },
    { NULL,         NULL    }
};


/**
 * Convert a string argument to a numeric, observing prefixes
 * for number bases, e.g. 0x for hex, 0 for octal
 *
 * @param mode          The CLI mode
 * @param arg           The string representation of the argument
 * @param arg_type      The target type for the argument
 * @return The argument as a long integer
 */
static unsigned long
convert_arg(int mode, char *arg, int arg_type)
{
    unsigned long  rval;
    SERVICE       *service;

    switch (arg_type)
    {
        case ARG_TYPE_ADDRESS:
            return (unsigned long)strtol(arg, NULL, 0);
        case ARG_TYPE_STRING:
            return (unsigned long)arg;
        case ARG_TYPE_SERVICE:
            if (mode == CLIM_USER || (rval = (unsigned long)strtol(arg, NULL, 0)) == 0)
            {
                rval = (unsigned long)service_find(arg);
            }
            return rval;
        case ARG_TYPE_SERVER:
            if (mode == CLIM_USER || (rval = (unsigned long)strtol(arg, NULL, 0)) == 0)
            {
                rval = (unsigned long)server_find_by_unique_name(arg);
            }
            return rval;
        case ARG_TYPE_DBUSERS:
            if (mode == CLIM_USER || (rval = (unsigned long)strtol(arg, NULL, 0)) == 0)
            {
                service = service_find(arg);
                if (service)
                {
                    return (unsigned long)(service->ports->users);
                }
                else
                {
                    return 0;
                }
            }
            return rval;
        case ARG_TYPE_DCB:
            rval = (unsigned long)strtol(arg, NULL, 0);
            if (mode == CLIM_USER && dcb_isvalid((DCB *)rval) == 0)
            {
                rval = 0;
            }
            return rval;
        case ARG_TYPE_SESSION:
            rval = (unsigned long)strtol(arg, NULL, 0);
            if (mode == CLIM_USER && session_isvalid((SESSION *)rval) == 0)
            {
                rval = 0;
            }
            return rval;
        case ARG_TYPE_MONITOR:
            if (mode == CLIM_USER || (rval = (unsigned long)strtol(arg, NULL, 0)) == 0)
            {
                rval = (unsigned long)monitor_find(arg);
            }
            return rval;
        case ARG_TYPE_FILTER:
            if (mode == CLIM_USER || (rval = (unsigned long)strtol(arg, NULL, 0)) == 0)
            {
                rval = (unsigned long)filter_find(arg);
            }
            return rval;
        case ARG_TYPE_NUMERIC:
        {
            int i;
            for (i = 0; arg[i]; i++)
            {
                if (arg[i] < '0' || arg[i] > '9')
                {
                    return 0;
                }
            }
            return atoi(arg);
        }
    }
    return 0;
}

/**
 * We have a complete line from the user, lookup the commands and execute them
 *
 * Commands are tokenised based on white space and then the first
 * word is checked againts the cmds table. If a match is found the
 * second word is compared to the different options for that command.
 *
 * Commands may also take up to 3 additional arguments, these are all
 * assumed to the numeric values and will be converted before being passed
 * to the handler function for the command.
 *
 * @param cli           The CLI_SESSION
 * @return      Returns 0 if the interpreter should exit
 */
int
execute_cmd(CLI_SESSION *cli)
{
    DCB           *dcb = cli->session->client_dcb;
    int            argc, i, j, found = 0;
    char          *args[MAXARGS + 1];
    int            in_quotes = 0, escape_next = 0;
    char          *ptr, *lptr;
    bool           in_space = false;
    int            nskip = 0;

    args[0] = cli->cmdbuf;
    ptr = args[0];
    lptr = ptr;
    i = 0;
    /*
     * Break the command line into a number of words. Whitespace is used
     * to delimit words and may be escaped by use of the \ character or
     * the use of double quotes.
     * The array args contains the broken down words, one per index.
     */

    while (*ptr)
    {
        if (escape_next)
        {
            *lptr++ = *ptr++;
            escape_next = 0;
        }
        else if (*ptr == '\\')
        {
            escape_next = 1;
            ptr++;
        }
        else if (in_quotes == 0 && ((in_space = *ptr == ' ') || *ptr == '\t' || *ptr == '\r' || *ptr == '\n'))
        {

            *lptr = 0;
            lptr += nskip;
            nskip = 0;

            if (!in_space)
            {
                break;
            }

            if (args[i] == ptr)
            {
                args[i] = ptr + 1;
            }
            else
            {
                i++;
                if (i >= MAXARGS - 1)
                {
                    break;
                }
                args[i] = ptr + 1;
            }
            ptr++;
            lptr++;
        }
        else if (*ptr == '\"' && in_quotes == 0)
        {
            in_quotes = 1;
            ptr++;
            nskip++;
        }
        else if (*ptr == '\"' && in_quotes == 1)
        {
            in_quotes = 0;
            ptr++;
            nskip++;
        }
        else
        {
            *lptr++ = *ptr++;
        }
    }
    *lptr = 0;
    args[MXS_MIN(MAXARGS - 1, i + 1)] = NULL;

    if (args[0] == NULL || *args[0] == 0)
    {
        return 1;
    }
    for (i = 0; args[i] && *args[i]; i++)
        ;
    argc = i - 2;   /* The number of extra arguments to commands */

    if (!strcasecmp(args[0], "help"))
    {
        if (args[1] == NULL || *args[1] == 0)
        {
            found = 1;
            dcb_printf(dcb, "Available commands:\n");
            for (i = 0; cmds[i].cmd; i++)
            {
                if (cmds[i].options[1].arg1 == NULL)
                {
                    dcb_printf(dcb, "    %s %s\n", cmds[i].cmd, cmds[i].options[0].arg1);
                }
                else
                {
                    dcb_printf(dcb, "    %s [", cmds[i].cmd);
                    for (j = 0; cmds[i].options[j].arg1; j++)
                    {
                        dcb_printf(dcb, "%s%s", cmds[i].options[j].arg1,
                                   cmds[i].options[j + 1].arg1 ? "|" : "");
                    }
                    dcb_printf(dcb, "]\n");
                }
            }
            dcb_printf(dcb, "\nType help command to see details of each command.\n");
            dcb_printf(dcb, "Where commands require names as arguments and these names contain\n");
            dcb_printf(dcb, "whitespace either the \\ character may be used to escape the whitespace\n");
            dcb_printf(dcb, "or the name may be enclosed in double quotes \".\n\n");
        }
        else
        {
            for (i = 0; cmds[i].cmd; i++)
            {
                if (!strcasecmp(args[1], cmds[i].cmd))
                {
                    found = 1;
                    dcb_printf(dcb, "Available options to the %s command:\n", args[1]);
                    for (j = 0; cmds[i].options[j].arg1; j++)
                    {
                        dcb_printf(dcb, "'%s' - %s\n\n\t%s\n\n", cmds[i].options[j].arg1,
                                   cmds[i].options[j].help, cmds[i].options[j].devhelp);

                    }
                }
            }
            if (found == 0)
            {
                dcb_printf(dcb, "No command %s to offer help with\n", args[1]);
            }
        }
        found = 1;
    }
    else if (!strcasecmp(args[0], "quit"))
    {
        return 0;
    }
    else if (argc >= 0)
    {
        for (i = 0; cmds[i].cmd; i++)
        {
            if (strcasecmp(args[0], cmds[i].cmd) == 0)
            {
                for (j = 0; cmds[i].options[j].arg1; j++)
                {
                    if (strcasecmp(args[1], cmds[i].options[j].arg1) == 0)
                    {
                        found = 1; /**< command and sub-command match */
                        if (argc < cmds[i].options[j].argc_min)
                        {
                            dcb_printf(dcb, "Incorrect number of arguments: %s %s expects at least %d arguments\n",
                                       cmds[i].cmd, cmds[i].options[j].arg1,
                                       cmds[i].options[j].argc_min);
                        }
                        else
                        {
                            unsigned long arg_list[MAXARGS] = {};

                            for (int k = 0; k < cmds[i].options[j].argc_max && k < argc; k++)
                            {
                                arg_list[k] = convert_arg(cli->mode, args[k + 2], cmds[i].options[j].arg_types[k]);
                                if (arg_list[k] == 0)
                                {
                                    dcb_printf(dcb, "Invalid argument: %s\n", args[k + 2]);
                                    return 0;
                                }
                            }

                            switch (cmds[i].options[j].argc_max)
                            {
                                case 0:
                                    cmds[i].options[j].fn(dcb);
                                    break;
                                case 1:
                                    cmds[i].options[j].fn(dcb, arg_list[0]);
                                    break;
                                case 2:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1]);
                                    break;
                                case 3:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2]);
                                    break;
                                case 4:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3]);
                                    break;
                                case 5:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4]);
                                    break;
                                case 6:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4], arg_list[5]);
                                    break;
                                case 7:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4], arg_list[5],
                                                          arg_list[6]);
                                    break;
                                case 8:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4], arg_list[5],
                                                          arg_list[6], arg_list[7]);
                                    break;
                                case 9:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4], arg_list[5],
                                                          arg_list[6], arg_list[7], arg_list[8]);
                                    break;
                                case 10:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4], arg_list[5],
                                                          arg_list[6], arg_list[7], arg_list[8],
                                                          arg_list[9]);
                                    break;
                                case 11:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4], arg_list[5],
                                                          arg_list[6], arg_list[7], arg_list[8],
                                                          arg_list[9], arg_list[10]);
                                    break;
                                case 12:
                                    cmds[i].options[j].fn(dcb, arg_list[0], arg_list[1], arg_list[2],
                                                          arg_list[3], arg_list[4], arg_list[5],
                                                          arg_list[6], arg_list[7], arg_list[8],
                                                          arg_list[9], arg_list[10], arg_list[11]);
                                    break;
                                default:
                                    dcb_printf(dcb, "Error: Maximum argument count is %d.\n", MAXARGS);
                                    break;
                            }
                        }
                    }
                }
                if (!found)
                {
                    dcb_printf(dcb,
                               "Unknown or missing option for the %s command. Valid sub-commands are:\n",
                               cmds[i].cmd);
                    for (j = 0; cmds[i].options[j].arg1; j++)
                    {
                        dcb_printf(dcb, "    %-10s %s\n", cmds[i].options[j].arg1,
                                   cmds[i].options[j].help);
                    }
                    found = 1;
                }
            }
        }
    }
    else if (argc == -1)
    {
        dcb_printf(dcb,
                   "Commands must consist of at least two words. Type help for a list of commands\n");
        found = 1;
    }
    if (!found)
    {
        dcb_printf(dcb,
                   "Command '%s' not known, type help for a list of available commands\n", args[0]);
    }

    memset(cli->cmdbuf, 0, CMDBUFLEN);

    return 1;
}

/**
 * Debug command to stop a service
 *
 * @param dcb           The DCB to print any output to
 * @param service       The service to shutdown
 */
static void
shutdown_service(DCB *dcb, SERVICE *service)
{
    serviceStop(service);
}

/**
 * Debug command to restart a stopped service
 *
 * @param dcb           The DCB to print any output to
 * @param service       The service to restart
 */
static void
restart_service(DCB *dcb, SERVICE *service)
{
    serviceRestart(service);
}

/**
 * Set the status bit of a server
 *
 * @param dcb           DCB to send output to
 * @param server        The server to set the status of
 * @param bit           String representation of the status bit
 */
static void
set_server(DCB *dcb, SERVER *server, char *bit)
{
    unsigned int bitvalue;

    if ((bitvalue = server_map_status(bit)) != 0)
    {
        server_set_status(server, bitvalue);
    }
    else
    {
        dcb_printf(dcb, "Unknown status bit %s\n", bit);
    }
}


/**
 * Clear the status bit of a server
 *
 * @param dcb           DCB to send output to
 * @param server        The server to set the status of
 * @param bit           String representation of the status bit
 */
static void
clear_server(DCB *dcb, SERVER *server, char *bit)
{
    unsigned int bitvalue;

    if ((bitvalue = server_map_status(bit)) != 0)
    {
        server_clear_status(server, bitvalue);
    }
    else
    {
        dcb_printf(dcb, "Unknown status bit %s\n", bit);
    }
}

/**
 * Reload the authenticaton data from the backend database of a service.
 *
 * @param dcb           DCB to send output
 * @param service       The service to update
 */
static void
reload_dbusers(DCB *dcb, SERVICE *service)
{
    if (service_refresh_users(service) == 0)
    {
        dcb_printf(dcb, "Reloaded database users for service %s.\n", service->name);
    }
    else
    {
        dcb_printf(dcb, "Error: Failed to reloaded database users for service %s.\n", service->name);
    }
}

/**
 * Relaod the configuration data from the config file
 *
 * @param dcb           DCB to use to send output
 */
static void
reload_config(DCB *dcb)
{
    dcb_printf(dcb, "Reloading configuration from file.\n");
    config_reload();
}

/**
 * Add a new remote (insecure, over the network) maxscale admin user
 *
 * @param dcb  The DCB for messages
 * @param user The user name
 * @param user The user password
 */
static void
telnetdAddUser(DCB *dcb, char *user, char *password)
{
    const char *err;

    if (admin_inet_user_exists(user))
    {
        dcb_printf(dcb, "Account %s for remote (network) usage already exists.\n", user);
        return;
    }

    if ((err = admin_add_inet_user(user, password)) == NULL)
    {
        dcb_printf(dcb, "Account %s for remote (network) usage has been successfully added.\n", user);
    }
    else
    {
        dcb_printf(dcb, "Failed to add new remote account %s: %s.\n", user, err);
    }
}

/**
 * Remove a remote (insecure, over the network) maxscale admin user
 *
 * @param dcb  The DCB for messages
 * @param user The user name
 * @param user The user password
 */
static void telnetdRemoveUser(DCB *dcb, char *user, char *password)
{
    const char* err;

    if (!admin_inet_user_exists(user))
    {
        dcb_printf(dcb, "Account %s for remote (network) usage does not exist.\n", user);
        return;
    }

    if ((err = admin_remove_inet_user(user, password)) == NULL)
    {
        dcb_printf(dcb, "Account %s for remote (network) usage has been successfully removed.\n", user);
    }
    else
    {
        dcb_printf(dcb, "Failed to remove remote account %s: %s\n", user, err);
    }
}


/**
 * Print the adminsitration users
 *
 * @param dcb   The DCB to print the user data to
 */
static void
telnetdShowUsers(DCB *dcb)
{
    dcb_PrintAdminUsers(dcb);
}

/**
 * Print the log throttling state
 *
 * @param dcb The DCB to print the state to.
 */
static void
show_log_throttling(DCB *dcb)
{
    MXS_LOG_THROTTLING t;
    mxs_log_get_throttling(&t);

    dcb_printf(dcb, "%lu %lu %lu\n", t.count, t.window_ms, t.suppress_ms);
}

/**
 * Command to shutdown a running monitor
 *
 * @param dcb   The DCB to use to print messages
 * @param monitor       The monitor to shutdown
 */
static void
shutdown_monitor(DCB *dcb, MONITOR *monitor)
{
    monitorStop(monitor);
}

/**
 * Command to restart a stopped monitor
 *
 * @param dcb   The DCB to use to print messages
 * @param monitor       The monitor to restart
 */
static void
restart_monitor(DCB *dcb, MONITOR *monitor)
{
    monitorStart(monitor, NULL);
}

/**
 * Enable replication heartbeat for a monitor
 *
 * @param dcb           Connection to user interface
 * @param monitor       The monitor
 */
static void
enable_monitor_replication_heartbeat(DCB *dcb, MONITOR *monitor)
{
    CONFIG_PARAMETER param;
    const char* name = "detect_replication_lag";
    const char* value = "1";
    param.name = (char*)name;
    param.value = (char*)value;
    param.next = NULL;
    monitorStop(monitor);
    monitorStart(monitor, &param);
}

/**
 * Disable replication heartbeat for a monitor
 *
 * @param dcb           Connection to user interface
 * @param monitor       The monitor
 */
static void
disable_monitor_replication_heartbeat(DCB *dcb, MONITOR *monitor)
{
    CONFIG_PARAMETER param;
    const char* name = "detect_replication_lag";
    const char* value = "0";
    param.name = (char*)name;
    param.value = (char*)value;
    param.next = NULL;
    monitorStop(monitor);
    monitorStart(monitor, &param);
}

/**
 * Enable root access to a service
 *
 * @param dcb           Connection to user interface
 * @param service       The service
 */
static void
enable_service_root(DCB *dcb, SERVICE *service)
{
    serviceEnableRootUser(service, 1);
}

/**
 * Disable root access to a service
 *
 * @param dcb           Connection to user interface
 * @param service       The service
 */
static void
disable_service_root(DCB *dcb, SERVICE *service)
{
    serviceEnableRootUser(service, 0);
}

struct log_action_entry
{
    const char* name;
    int priority;
    const char* replacement;
};

static bool get_log_action(const char* name, struct log_action_entry* entryp)
{
    static const struct log_action_entry entries[] =
    {
        { "debug",   LOG_DEBUG,  "debug" },
        { "trace",   LOG_INFO,   "info" },
        { "message", LOG_NOTICE, "notice" },
    };
    const int n_entries = sizeof(entries) / sizeof(entries[0]);

    bool found = false;
    int i = 0;

    while (!found && (i < n_entries))
    {
        if (strcmp(name, entries[i].name) == 0)
        {
            *entryp = entries[i];
            found = true;
        }

        ++i;
    }

    return found;
}

/**
 * Enables a log for a single session
 * @param session The session in question
 * @param dcb Client DCB
 * @param type Which log to enable
 */
static void enable_sess_log_action(DCB *dcb, char *arg1, char *arg2)
{
    struct log_action_entry entry;

    if (get_log_action(arg1, &entry))
    {
        size_t id = (size_t) strtol(arg2, 0, 0);
        list_entry_t *current = list_start_iteration(&SESSIONlist);
        while (current)
        {
            SESSION *session = (SESSION *)current;
            if (session->ses_id == id)
            {
                session_enable_log_priority(session, entry.priority);
                list_terminate_iteration_early(&SESSIONlist, current);
                break;
            }
            current = list_iterate(&SESSIONlist, current);
        }

        if (!current)
        {
            dcb_printf(dcb, "Session not found: %s.\n", arg2);
        }
    }
    else
    {
        dcb_printf(dcb, "%s is not supported for enable log.\n", arg1);
    }
}

/**
 * Disables a log for a single session
 * @param session The session in question
 * @param dcb Client DCB
 * @param type Which log to disable
 */
static void disable_sess_log_action(DCB *dcb, char *arg1, char *arg2)
{
    struct log_action_entry entry;

    if (get_log_action(arg1, &entry))
    {
        size_t id = (size_t) strtol(arg2, 0, 0);
        list_entry_t *current = list_start_iteration(&SESSIONlist);
        while (current)
        {
            SESSION *session = (SESSION *)current;
            if (session->ses_id == id)
            {
                session_disable_log_priority(session, entry.priority);
                list_terminate_iteration_early(&SESSIONlist, current);
                break;
            }
            current = list_iterate(&SESSIONlist, current);
        }

        if (!current)
        {
            dcb_printf(dcb, "Session not found: %s.\n", arg2);
        }
    }
    else
    {
        dcb_printf(dcb, "%s is not supported for disable log.\n", arg1);
    }
}

struct log_priority_entry
{
    const char* name;
    int priority;
};

static int compare_log_priority_entries(const void* l, const void* r)
{
    const struct log_priority_entry* l_entry = (const struct log_priority_entry*) l;
    const struct log_priority_entry* r_entry = (const struct log_priority_entry*) r;

    return strcmp(l_entry->name, r_entry->name);
}

static int string_to_priority(const char* name)
{
    static const struct log_priority_entry LOG_PRIORITY_ENTRIES[] =
    {
        // NOTE: If you make changes to this array, ensure that it remains alphabetically ordered.
        { "debug",   LOG_DEBUG },
        { "info",    LOG_INFO },
        { "notice",  LOG_NOTICE },
        { "warning", LOG_WARNING },
    };

    const size_t N_LOG_PRIORITY_ENTRIES = sizeof(LOG_PRIORITY_ENTRIES) / sizeof(LOG_PRIORITY_ENTRIES[0]);

    struct log_priority_entry key = { name, -1 };
    struct log_priority_entry* result = bsearch(&key,
                                                LOG_PRIORITY_ENTRIES,
                                                N_LOG_PRIORITY_ENTRIES,
                                                sizeof(struct log_priority_entry),
                                                compare_log_priority_entries);

    return result ? result->priority : -1;
}

/**
 * Enables a log priority for a single session
 * @param session The session in question
 * @param dcb Client DCB
 * @param type Which log to enable
 */
static void enable_sess_log_priority(DCB *dcb, char *arg1, char *arg2)
{
    int priority = string_to_priority(arg1);

    if (priority != -1)
    {
        size_t id = (size_t) strtol(arg2, 0, 0);
        list_entry_t *current = list_start_iteration(&SESSIONlist);
        while (current)
        {
            SESSION *session = (SESSION *)current;
            if (session->ses_id == id)
            {
                session_enable_log_priority(session, priority);
                list_terminate_iteration_early(&SESSIONlist, current);
                break;
            }
            current = list_iterate(&SESSIONlist, current);
        }

        if (!current)
        {
            dcb_printf(dcb, "Session not found: %s.\n", arg2);
        }
    }
    else
    {
        dcb_printf(dcb, "'%s' is not a supported log priority.\n", arg1);
    }
}

/**
 * Disable a log priority for a single session
 * @param session The session in question
 * @param dcb Client DCB
 * @param type Which log to enable
 */
static void disable_sess_log_priority(DCB *dcb, char *arg1, char *arg2)
{
    int priority = string_to_priority(arg1);

    if (priority != -1)
    {
        size_t id = (size_t) strtol(arg2, 0, 0);
        list_entry_t *current = list_start_iteration(&SESSIONlist);
        while (current)
        {
            SESSION *session = (SESSION *)current;
            if (session->ses_id == id)
            {
                session_disable_log_priority(session, priority);
                list_terminate_iteration_early(&SESSIONlist, current);
                break;
            }
            current = list_iterate(&SESSIONlist, current);
        }

        if (!current)
        {
            dcb_printf(dcb, "Session not found: %s.\n", arg2);
        }
    }
    else
    {
        dcb_printf(dcb, "'%s' is not a supported log priority.\n", arg1);
    }
}

/**
 * The log enable action
 */
static void enable_log_action(DCB *dcb, char *arg1)
{
    struct log_action_entry entry;

    if (get_log_action(arg1, &entry))
    {
        mxs_log_set_priority_enabled(entry.priority, true);

        dcb_printf(dcb,
                   "'enable log %s' is accepted but deprecated, use 'enable log-priority %s' instead.\n",
                   arg1, entry.replacement);
    }
    else
    {
        dcb_printf(dcb, "'%s' is not supported for enable log.\n", arg1);
    }
}

/**
 * The log disable action
 */
static void disable_log_action(DCB *dcb, char *arg1)
{
    struct log_action_entry entry;

    if (get_log_action(arg1, &entry))
    {
        mxs_log_set_priority_enabled(entry.priority, false);

        dcb_printf(dcb,
                   "'disable log %s' is accepted but deprecated, use 'enable log-priority %s' instead.\n",
                   arg1, entry.replacement);
    }
    else
    {
        dcb_printf(dcb, "'%s' is not supported for 'disable log'.\n", arg1);
    }
}

/**
 * The log-priority enable action
 */

static void enable_log_priority(DCB *dcb, char *arg1)
{
    int priority = string_to_priority(arg1);

    if (priority != -1)
    {
        mxs_log_set_priority_enabled(priority, true);
    }
    else
    {
        dcb_printf(dcb, "'%s' is not a supported log priority.\n", arg1);
    }
}

/**
 * The log-priority disable action
 */

static void disable_log_priority(DCB *dcb, char *arg1)
{
    int priority = string_to_priority(arg1);

    if (priority != -1)
    {
        mxs_log_set_priority_enabled(priority, false);
    }
    else
    {
        dcb_printf(dcb, "'%s' is not a supported log priority.\n", arg1);
    }
}

/**
 * Set the duration of the sleep passed to the poll wait
 *
 * @param       dcb             DCB for output
 * @param       sleeptime       Sleep time in milliseconds
 */
static void
set_pollsleep(DCB *dcb, int sleeptime)
{
    poll_set_maxwait(sleeptime);
}

/**
 * Set the number of non-blockign spins to make
 *
 * @param       dcb             DCB for output
 * @param       nb              Number of spins
 */
static void
set_nbpoll(DCB *dcb, int nb)
{
    poll_set_nonblocking_polls(nb);
}

static void
set_log_throttling(DCB *dcb, int count, int window_ms, int suppress_ms)
{
    if ((count >= 0) || (window_ms >= 0) || (suppress_ms >= 0))
    {
        MXS_LOG_THROTTLING t = { count, window_ms, suppress_ms };

        mxs_log_set_throttling(&t);
    }
    else
    {
        dcb_printf(dcb,
                   "set log_throttling expect 3 integers X Y Z, equal to or larger than 0, "
                   "where the X denotes how many times particular message may be logged "
                   "during a period of Y milliseconds before it is suppressed for Z milliseconds.");
    }
}

/**
 * Re-enable sendig MaxScale module list via http
 * Proper [feedback] section in MaxSclale.cnf
 * is required.
 */
static void
enable_feedback_action(void)
{
    config_enable_feedback_task();
    return;
}

/**
 * Disable sendig MaxScale module list via http
 */

static void
disable_feedback_action(void)
{
    config_disable_feedback_task();
    return;
}

/**
 * Enable syslog logging.
 */
static void
enable_syslog()
{
    mxs_log_set_syslog_enabled(true);
}

/**
 * Disable syslog logging.
 */
static void
disable_syslog()
{
    mxs_log_set_syslog_enabled(false);
}

/**
 * Enable maxlog logging.
 */
static void
enable_maxlog()
{
    mxs_log_set_maxlog_enabled(true);
}

/**
 * Disable maxlog logging.
 */
static void
disable_maxlog()
{
    mxs_log_set_maxlog_enabled(false);
}

/**
 * Enable a Linux account
 *
 * @param dcb  The DCB for messages
 * @param user The Linux user name
 */
static void
enable_account(DCB *dcb, char *user)
{
    const char *err;

    if (admin_linux_account_enabled(user))
    {
        dcb_printf(dcb, "The Linux user %s has already been enabled.\n", user);
        return;
    }

    if ((err = admin_enable_linux_account(user)) == NULL)
    {
        dcb_printf(dcb, "The Linux user %s has successfully been enabled.\n", user);
    }
    else
    {
        dcb_printf(dcb, "Failed to enable the Linux user %s: %s\n", user, err);
    }
}

/**
 * Disable a Linux account
 *
 * @param dcb   The DCB for messages
 * @param user  The Linux user name
 */
static void
disable_account(DCB *dcb, char *user)
{
    const char* err;

    if (!admin_linux_account_enabled(user))
    {
        dcb_printf(dcb, "The Linux user %s has not been enabled.\n", user);
        return;
    }

    if ((err = admin_disable_linux_account(user)) == NULL)
    {
        dcb_printf(dcb, "The Linux user %s has successfully been disabled.\n", user);
    }
    else
    {
        dcb_printf(dcb, "Failed to disable the Linux user %s: %s\n", user, err);
    }
}

#if defined(FAKE_CODE)
static void fail_backendfd(void)
{
    fail_next_backend_fd = true;
}

static void fail_clientfd(void)
{
    fail_next_client_fd = true;
}

static void fail_accept(
    DCB*  dcb,
    char* arg1,
    char* arg2)
{
    int failcount = MXS_MIN(atoi(arg2), 100);
    fail_accept_errno = atoi(arg1);
    char errbuf[MXS_STRERROR_BUFLEN];

    switch (fail_accept_errno)
    {
        case EAGAIN:
//    case EWOULDBLOCK:
        case EBADF:
        case EINTR:
        case EINVAL:
        case EMFILE:
        case ENFILE:
        case ENOTSOCK:
        case EOPNOTSUPP:
        case ENOBUFS:
        case ENOMEM:
        case EPROTO:
            fail_next_accept = failcount;
            break;

        default:
            dcb_printf(dcb,
                       "[%d, %s] is not valid errno for accept.\n",
                       fail_accept_errno,
                       strerror_r(fail_accept_errno, errbuf, sizeof(errbuf)));
            return ;
    }
}
#endif /* FAKE_CODE */
