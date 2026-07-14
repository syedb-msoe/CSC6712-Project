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
    explicit ServerWrapper(int txn_timeout_ms = 30000) {
        int id = global_counter.fetch_add(1);
        db_ = get_directory() + "/server_test_data" + std::to_string(id) + ".db";
        wal_ = get_directory() + "/server_test_data" + std::to_string(id) + ".wal";
        std::remove(db_.c_str());
        std::remove(wal_.c_str());
        BTreeDB::create(db_);

        server_ = std::make_unique<BTreeServer>(db_, wal_);
        server_->set_txn_timeout_ms(txn_timeout_ms);
        EXPECT_TRUE(server_->listen_on(0));
        port_ = server_->bound_port();
        thread_ = std::thread([this] { server_->run(); });
    }

    ~ServerWrapper() {
        stop();
        std::remove(db_.c_str());
        std::remove(wal_.c_str());
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

    uint16_t port() const { return port_; }
    const std::string& db_path() const { return db_; }
    const std::string& wal_path() const { return wal_; }

private:
    std::string db_;
    std::string wal_;
    uint16_t port_ = 0;
    std::unique_ptr<BTreeServer> server_;
    std::thread thread_;
};

TEST(ServerTest, PutGetContains) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->put("alpha", "1"), "OK NULL");
    EXPECT_EQ(c->get("alpha"), "VALUE 1");
    EXPECT_EQ(c->contains("alpha"), "TRUE");

    EXPECT_EQ(c->get("missing"), "NULL");
    EXPECT_EQ(c->contains("missing"), "FALSE");
}

TEST(ServerTest, PutReturnsPreviousValue) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->put("k", "old"), "OK NULL");
    EXPECT_EQ(c->put("k", "new"), "OK old");
    EXPECT_EQ(c->get("k"), "VALUE new");
}

TEST(ServerTest, ProtocolErrors) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->command("INVALIDCOMMAND test"), "ERR PROTOCOL");
    EXPECT_EQ(c->command("PUT onlyonekey"), "ERR PROTOCOL");
    EXPECT_EQ(c->command("GET"), "ERR PROTOCOL");
    EXPECT_EQ(c->command(""), "ERR PROTOCOL");
}

TEST(ServerTest, CommitFlushesBufferedWrites) {
    ServerWrapper h;
    auto writer = h.client();
    auto other = h.client();

    ASSERT_EQ(writer->begin({"alpha", "beta", "gamma"}).substr(0, 2), "OK");
    EXPECT_EQ(writer->put("alpha", "0"), "OK NULL");
    EXPECT_EQ(writer->put("beta", "1"), "OK NULL");
    EXPECT_EQ(writer->put("alpha", "1"), "OK 0");

    EXPECT_EQ(other->get("alpha"), "ERR KEY_LOCKED");

    EXPECT_EQ(writer->commit(), "OK");

    EXPECT_EQ(other->get("alpha"), "VALUE 1");
    EXPECT_EQ(other->get("beta"), "VALUE 1");
}

TEST(ServerTest, AbortDiscardsBufferedWrites) {
    ServerWrapper h;
    auto c = h.client();

    ASSERT_EQ(c->put("y", "orig"), "OK NULL");
    ASSERT_EQ(c->begin({"y"}).substr(0, 2), "OK");
    EXPECT_EQ(c->put("y", "changed"), "OK orig");
    EXPECT_EQ(c->abort(), "OK");
    EXPECT_EQ(c->get("y"), "VALUE orig");
}

TEST(ServerTest, LockConflictOnBegin) {
    ServerWrapper h;
    auto a = h.client();
    auto b = h.client();

    ASSERT_EQ(a->begin({"shared", "a_only"}).substr(0, 2), "OK");
    EXPECT_EQ(b->begin({"shared", "b_only"}), "ERR LOCK_FAILED");
    EXPECT_EQ(b->begin({"b_only"}).substr(0, 2), "OK");
    EXPECT_EQ(a->abort(), "OK");
    EXPECT_EQ(b->abort(), "OK");
}

TEST(ServerTest, CommitWithNoWritesFails) {
    ServerWrapper h;
    auto c = h.client();

    ASSERT_EQ(c->begin({"z"}).substr(0, 2), "OK");
    EXPECT_EQ(c->get("z"), "NULL");
    EXPECT_EQ(c->commit(), "ERR NO_WRITES");
    EXPECT_EQ(c->abort(), "OK");
}

TEST(ServerTest, UnexpectedTransactionCommands) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->commit(), "ERR NO_TXN");
    EXPECT_EQ(c->abort(), "ERR NO_TXN");

    ASSERT_EQ(c->begin({"k"}).substr(0, 2), "OK");
    EXPECT_EQ(c->begin({"k"}), "ERR TXN_ACTIVE");
    EXPECT_EQ(c->get("other"), "ERR KEY_NOT_LOCKED");
    EXPECT_EQ(c->put("other", "v"), "ERR KEY_NOT_LOCKED");
    EXPECT_EQ(c->abort(), "OK");
}

TEST(ServerTest, ExpirationRequiresAbort) {
    ServerWrapper h(100);
    auto c = h.client();

    std::string resp = c->begin({"e"});
    ASSERT_EQ(resp.substr(0, 2), "OK");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    EXPECT_EQ(c->put("e", "v"), "ERR TXN_EXPIRED");
    EXPECT_EQ(c->get("e"), "ERR TXN_EXPIRED");
    EXPECT_EQ(c->commit(), "ERR TXN_EXPIRED");
    EXPECT_EQ(c->abort(), "OK");

    EXPECT_EQ(c->begin({"e"}).substr(0, 2), "OK");
    EXPECT_EQ(c->abort(), "OK");
}

TEST(ServerTest, IndependentConnectionsInterleave) {
    ServerWrapper h;
    auto a = h.client();
    auto b = h.client();

    EXPECT_EQ(a->put("a1", "1"), "OK NULL");
    EXPECT_EQ(b->put("b1", "2"), "OK NULL");
    EXPECT_EQ(a->get("b1"), "VALUE 2");
    EXPECT_EQ(b->get("a1"), "VALUE 1");
}

TEST(ServerTest, CommittedDataSurvivesRestart) {
    int id = global_counter.fetch_add(1);
    std::string db = get_directory() + "/rec_" + std::to_string(id) + ".db";
    std::string wal = get_directory() + "/rec_" + std::to_string(id) + ".wal";
    std::remove(db.c_str());
    std::remove(wal.c_str());
    BTreeDB::create(db);

    {
        BTreeServer server(db, wal);
        ASSERT_TRUE(server.listen_on(0));
        uint16_t port = server.bound_port();
        std::thread th([&] { server.run(); });

        Client c;
        ASSERT_TRUE(c.connect("127.0.0.1", port));
        ASSERT_EQ(c.begin({"persist"}).substr(0, 2), "OK");
        ASSERT_EQ(c.put("persist", "value"), "OK NULL");
        ASSERT_EQ(c.commit(), "OK");
        c.disconnect();

        server.request_shutdown();
        th.join();
    }

    {
        BTreeServer server(db, wal);
        ASSERT_TRUE(server.listen_on(0));
        uint16_t port = server.bound_port();
        std::thread th([&] { server.run(); });

        Client c;
        ASSERT_TRUE(c.connect("127.0.0.1", port));
        EXPECT_EQ(c.get("persist"), "VALUE value");
        c.disconnect();

        server.request_shutdown();
        th.join();
    }

    std::remove(db.c_str());
    std::remove(wal.c_str());
}

TEST(ServerTest, ShutdownCommandClosesServer) {
    ServerWrapper h;
    auto c = h.client();

    EXPECT_EQ(c->put("k", "v"), "OK NULL");
    EXPECT_EQ(c->shutdown(), "OK SHUTTING_DOWN");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h.stop();
    SUCCEED();
}
