#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "MurmurHash3.h"

class RendezvousHasher {
public:
    explicit RendezvousHasher(std::vector<std::string> servers,
                              uint32_t seed = 0)
        : servers_(std::move(servers)), seed_(seed) {}

    const std::vector<std::string>& servers() const { return servers_; }
    size_t size() const { return servers_.size(); }

    std::vector<size_t> replica_set(const std::string& key, size_t replica_size) const {
        std::vector<std::pair<uint32_t, size_t>> weighted;
        weighted.reserve(servers_.size());
        for (size_t i = 0; i < servers_.size(); ++i) {
            weighted.emplace_back(weight(key, servers_[i]), i);
        }

        std::sort(weighted.begin(), weighted.end(),
                  [this](const std::pair<uint32_t, size_t>& a,
                         const std::pair<uint32_t, size_t>& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return servers_[a.second] < servers_[b.second];
                  });

        if (replica_size > weighted.size()) replica_size = weighted.size();
        std::vector<size_t> out;
        out.reserve(replica_size);
        for (size_t i = 0; i < replica_size; ++i) out.push_back(weighted[i].second);
        return out;
    }

    uint32_t weight(const std::string& key, const std::string& server) const {
        std::string buf;
        buf.reserve(key.size() + 1 + server.size());
        buf.append(key);
        buf.push_back('\0');
        buf.append(server);

        uint32_t out = 0;
        MurmurHash3_x86_32(buf.data(), static_cast<int>(buf.size()), seed_,
                           &out);
        return out;
    }

private:
    std::vector<std::string> servers_;
    uint32_t seed_;
};