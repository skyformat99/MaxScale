#include <future>
#include <iostream>
#include <librdkafka/rdkafkacpp.h>
#include "testconnections.h"

using namespace std::literals::string_literals;

std::unique_ptr<RdKafka::KafkaConsumer> consumer;

void prepare_consumer(TestConnections& test)
{
    std::string err;
    auto cnf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);
    cnf->set("bootstrap.servers", test.maxscales->IP[0] + ":9092"s, err);
    cnf->set("group.id", "kafkacdc", err);

    consumer.reset(RdKafka::KafkaConsumer::create(cnf, err));
    consumer->subscribe({"kafkacdc"});
    delete cnf;
}

int consume_messages(TestConnections& test)
{
    int i = 0;
    bool ok = true;

    while (ok)
    {
        auto msg = consumer->consume(10000);

        if (msg->err() == RdKafka::ERR_NO_ERROR)
        {
            std::cout << "Message key: " << *msg->key() << std::endl;
            std::cout << "Message content: " << std::string((char*)msg->payload(), msg->len()) << std::endl;
            i++;
        }
        else
        {
            ok = false;
        }

        delete msg;
    }

    return i;
}

void read_messages(TestConnections& test, int n_expected)
{
    int i = consume_messages(test);
    test.expect(i == n_expected, "Expected %d messages, got %d", n_expected, i);
}

int main(int argc, char** argv)
{
    TestConnections test(argc, argv);

    test.tprintf("Starting Kafka container");
    auto res = test.maxscales->ssh_output(
        "sudo docker run -d -e ADVERTISED_HOST="s + test.maxscales->IP[0]
        + " -p 9092:9092 -p 2182:2181 --name=kafka spotify/kafka");

    if (res.first != 0)
    {
        test.tprintf("Failed to start docker container: %s", res.second.c_str());
        return 1;
    }

    test.repl->stop_slaves();
    auto conn = test.repl->get_connection(0);

    test.tprintf("Inserting data");
    conn.connect();
    conn.query("CREATE TABLE t1(id INT)");
    conn.query("INSERT INTO t1 VALUES (1), (2), (3)");
    conn.query("UPDATE t1 SET id = 4 WHERE id = 2");
    conn.query("DELETE FROM t1 WHERE id = 3");
    auto gtid = conn.field("SELECT @@gtid_binlog_pos");

    test.tprintf("Give MaxScale some time to process the events");
    sleep(5);

    // Connect to Kafka
    prepare_consumer(test);

    read_messages(test, 7);

    conn.query("INSERT INTO t1 VALUES (4), (5), (6)");
    sleep(5);

    read_messages(test, 3);

    test.tprintf("Restarting MaxScale and inserting data");
    test.maxscales->stop();
    test.maxscales->ssh_output("rm /var/lib/maxscale/Kafka-CDC/current_gtid.txt");
    test.maxscales->start();

    conn.query("INSERT INTO t1 VALUES (7), (8), (9)");
    sleep(5);

    read_messages(test, 3);

    test.tprintf("Stopping Kafka container");
    test.maxscales->ssh_output("sudo docker rm -vf kafka");
    test.repl->fix_replication();

    return test.global_result;
}
