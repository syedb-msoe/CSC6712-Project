#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "rendezvous.h"

std::vector<std::string> make_servers(int n) {
    std::vector<std::string> s;
    for (int i = 0; i < n; ++i) {
        s.push_back("127.0.0.1:" + std::to_string(25120 + i));
    }
    return s;
}

TEST(RendezvousTest, ReplicaSetHasRequestedSize) {
    RendezvousHasher h(make_servers(5));
    for (int i = 0; i < 100; ++i) {
        auto rs = h.replica_set("key_" + std::to_string(i), 3);
        EXPECT_EQ(rs.size(), 3u);
        std::set<size_t> uniq(rs.begin(), rs.end());
        EXPECT_EQ(uniq.size(), rs.size());
    }
}

TEST(RendezvousTest, DistributesKeysEvenlyAcrossServers) {
    RendezvousHasher h(make_servers(5));
    std::vector<int> primary_counts(5, 0); // track number of replicated keys on each server
    const int keys = 5000;
    for (int i = 0; i < keys; ++i) {
        auto rs = h.replica_set("k" + std::to_string(i), 1);
        primary_counts[rs[0]]++;
    }
    for (int c : primary_counts) {
        EXPECT_GT(c, keys / 5 / 1.25); // divide by 1.25 is for tolerance since hash may not be perfectly uniform
    }
}

TEST(RendezvousTest, ReplicatesKeysAcrossMultipleServers) {
    const int servers = 5;
    const size_t replica_size = 3;
    RendezvousHasher h(make_servers(servers));

    std::vector<int> replica_counts(servers, 0);
    const int keys = 5000;
    for (int i = 0; i < keys; ++i) {
        auto rs = h.replica_set("k" + std::to_string(i), replica_size);
        ASSERT_EQ(rs.size(), replica_size);
        std::set<size_t> distinct(rs.begin(), rs.end());
        ASSERT_EQ(distinct.size(), replica_size);

        for (size_t idx : rs) replica_counts[idx]++;
    }

    const int expected = keys * static_cast<int>(replica_size) / servers;
    for (int c : replica_counts) {
        EXPECT_GT(c, expected / 1.25);
    }
}

TEST(RendezvousTest, RemovalOnlyRehomesAffectedKeys) {
    auto servers = make_servers(5);
    RendezvousHasher full(servers);

    std::vector<std::string> reduced;
    for (size_t i = 0; i < servers.size(); ++i) {
        if (i != 2) reduced.push_back(servers[i]);
    }
    RendezvousHasher smaller(reduced);

    for (int i = 0; i < 500; ++i) {
        std::string k = "key" + std::to_string(i);

        auto full_rank = full.replica_set(k, servers.size());
        std::vector<std::string> full_addrs;
        for (size_t idx : full_rank) full_addrs.push_back(servers[idx]);

        std::vector<std::string> expected;
        for (const std::string& a : full_addrs) {
            if (a != servers[2]) expected.push_back(a);
        }

        auto small_rank = smaller.replica_set(k, reduced.size());
        std::vector<std::string> small_addrs;
        for (size_t idx : small_rank) small_addrs.push_back(reduced[idx]);

        EXPECT_EQ(small_addrs, expected) << "key=" << k;
    }
}