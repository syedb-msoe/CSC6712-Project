#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "btree.h"
#include "btree_server.h"
#include "client.h"
#include "dist_client.h"
#include "rendezvous.h"

std::string get_directory() {
    std::string home = std::getenv("HOME");
    std::string dir = home + "/test_data";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::atomic<int> dist_counter{0};

class Node {
public:
    Node() {
        int id = dist_counter.fetch_add(1);
        db_ = get_directory() + "/dist_test" + std::to_string(id) + ".db";
        wal_ = get_directory() + "/dist_test" + std::to_string(id) + ".wal";
        std::remove(db_.c_str());
        std::remove(wal_.c_str());
        BTreeDB::create(db_);

        server_ = std::make_unique<BTreeServer>(db_, wal_);
        server_->listen_on(0);
        port_ = server_->bound_port();
        thread_ = std::thread([this] { server_->run(); });
    }

    ~Node() {
        stop();
        std::remove(db_.c_str());
        std::remove(wal_.c_str());
    }

    void stop() {
        if (server_) server_->request_shutdown();
        if (thread_.joinable()) thread_.join();
        server_.reset();
    }

    std::string endpoint() const {
        return "127.0.0.1:" + std::to_string(port_);
    }
    uint16_t port() const { return port_; }

    std::unique_ptr<Client> client() {
        auto c = std::make_unique<Client>();
        c->connect("127.0.0.1", port_);
        return c;
    }

private:
    std::string db_;
    std::string wal_;
    uint16_t port_ = 0;
    std::unique_ptr<BTreeServer> server_;
    std::thread thread_;
};

class Cluster {
public:
    explicit Cluster(int n) {
        for (int i = 0; i < n; ++i) nodes_.push_back(std::make_unique<Node>());
    }

    std::vector<std::string> endpoints() const {
        std::vector<std::string> eps;
        for (const auto& n : nodes_) eps.push_back(n->endpoint());
        return eps;
    }

    Node& node(size_t i) { return *nodes_[i]; }
    size_t size() const { return nodes_.size(); }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
};

TEST(DistClientTest, DefaultQuorumForR3) {
    Cluster cluster(3);
    DistributedClient db(3, cluster.endpoints());
    EXPECT_EQ(db.replica_size(), 3u);
    EXPECT_EQ(db.write_quorum(), 2);
    EXPECT_EQ(db.read_quorum(), 2);
    EXPECT_EQ(db.reachable(), 3);
}

TEST(DistClientTest, InvalidQuorumRejected) {
    Cluster cluster(3);
    EXPECT_THROW(DistributedClient(3, cluster.endpoints(), 3, 1),
                 std::invalid_argument);
    EXPECT_THROW(DistributedClient(5, cluster.endpoints()),
                 std::invalid_argument);
}

TEST(DistClientTest, PutThenGet) {
    Cluster cluster(3);
    DistributedClient db(3, cluster.endpoints());

    auto p = db.put("alpha", "42");
    EXPECT_EQ(p.status, DistributedClient::PutResult::COMMITTED);
    EXPECT_EQ(p.committed, 3);

    auto g = db.get("alpha");
    EXPECT_EQ(g.status, DistributedClient::GetResult::OK);
    EXPECT_EQ(g.value, "42");
    EXPECT_GE(g.agree, db.read_quorum());
}

TEST(DistClientTest, MissingKeyIsConsistentNotFound) {
    Cluster cluster(3);
    DistributedClient db(3, cluster.endpoints());

    auto g = db.get("nonexistent");
    EXPECT_EQ(g.status, DistributedClient::GetResult::NOT_FOUND);
    EXPECT_GE(g.agree, db.read_quorum());
}

TEST(DistClientTest, SurvivesOneDeadReplica) {
    Cluster cluster(3);
    DistributedClient db(3, cluster.endpoints());

    ASSERT_EQ(db.put("k", "v1").status,
              DistributedClient::PutResult::COMMITTED);

    RendezvousHasher h(cluster.endpoints());
    size_t primary = h.replica_set("k", 1)[0];
    cluster.node(primary).stop();

    auto p = db.put("k", "v2");
    EXPECT_EQ(p.status, DistributedClient::PutResult::COMMITTED);
    EXPECT_GE(p.committed, db.write_quorum());

    auto g = db.get("k");
    EXPECT_EQ(g.status, DistributedClient::GetResult::OK);
    EXPECT_EQ(g.value, "v2");
}

TEST(DistClientTest, WriteAbortsWhenQuorumUnreachable) {
    Cluster cluster(3);
    DistributedClient db(3, cluster.endpoints());

    ASSERT_EQ(db.put("k", "v1").status,
              DistributedClient::PutResult::COMMITTED);

    cluster.node(0).stop();
    cluster.node(1).stop();

    auto p = db.put("k", "v2");
    EXPECT_EQ(p.status, DistributedClient::PutResult::ABORTED_CONSISTENT);

    auto g = db.get("k");
    EXPECT_EQ(g.status, DistributedClient::GetResult::INCONSISTENT);
}

TEST(DistClientTest, DivergentReplicasReportInconsistent) {
    Cluster cluster(3);
    DistributedClient db(3, cluster.endpoints());

    cluster.node(0).client()->put("key", "A");
    cluster.node(1).client()->put("key", "B");

    auto g = db.get("key");
    EXPECT_EQ(g.status, DistributedClient::GetResult::INCONSISTENT);
}

TEST(DistClientTest, ReplicatesToExactlyReplicaSet) {
    Cluster cluster(5);
    DistributedClient db(3, cluster.endpoints());

    ASSERT_EQ(db.put("obj", "payload").status,
              DistributedClient::PutResult::COMMITTED);

    RendezvousHasher h(cluster.endpoints());
    std::vector<size_t> rs = h.replica_set("obj", 3);
    std::set<size_t> replicas(rs.begin(), rs.end());
    ASSERT_EQ(replicas.size(), 3u);

    int holders = 0;
    for (size_t i = 0; i < cluster.size(); ++i) {
        std::string resp = cluster.node(i).client()->get("obj");
        if (replicas.count(i)) {
            EXPECT_EQ(resp, "VALUE payload") << "expected replica " << i;
            ++holders;
        } else {
            EXPECT_EQ(resp, "NULL") << "non-replica " << i << " should not hold key";
        }
    }
    EXPECT_EQ(holders, 3);
}