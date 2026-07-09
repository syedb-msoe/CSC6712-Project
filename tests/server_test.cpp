#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "btree.h"
#include "btree_server.h"
#include "client.h"

std::string get_directory() {
    std::string home = std::getenv("HOME");
    std::string dir = home + "/test_data";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::atomic<int> global_counter{0};

class ServerWrapper {
public:
    explicit ServerWrapper() {
        int id = global_counter.fetch_add(1);
        db_ = get_directory() + "/server_test_data" + std::to_string(id) + ".db";
        std::remove(db_.c_str());
        BTreeDB::create(db_);

        server_ = std::make_unique<BTreeServer>(db_);
        EXPECT_TRUE(server_->listen_on(0));
        port_ = server_->bound_port();
        thread_ = std::thread([this] { server_->run(); });
    }

    ~ServerWrapper() {
        stop();
        std::remove(db_.c_str());
    }

    void stop() {
        if (server_) server_->request_shutdown();
        if (thread_.joinable()) thread_.join();
    }

    std::unique_ptr<Client> client() {
        auto c = std::make_unique<Client>();
        EXPECT_TRUE(c->connect("127.0.0.1", port_));
        return c;
    }

private:
    std::string db_;
    uint16_t port_ = 0;
    std::unique_ptr<BTreeServer> server_;
    std::thread thread_;
};

TEST(ServerBasic, PutGetContains) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->put("alpha", "1"), "OK NULL");
    EXPECT_EQ(c->get("alpha"), "VALUE 1");
    EXPECT_EQ(c->contains("alpha"), "TRUE");

    EXPECT_EQ(c->get("missing"), "NULL");
    EXPECT_EQ(c->contains("missing"), "FALSE");
}

TEST(ServerBasic, PutReturnsPreviousValue) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->put("k", "old"), "OK NULL");
    EXPECT_EQ(c->put("k", "new"), "OK old");
    EXPECT_EQ(c->get("k"), "VALUE new");
}

TEST(ServerBasic, ProtocolErrors) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->command("FOO bar"), "ERR PROTOCOL");
    EXPECT_EQ(c->command("PUT onlyonekey"), "ERR PROTOCOL");
    EXPECT_EQ(c->command("GET"), "ERR PROTOCOL");
    EXPECT_EQ(c->command(""), "ERR PROTOCOL");
}