#include "dist_client.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

bool parse_endpoint(const std::string& ep, std::string& host, uint16_t& port) {
    std::string::size_type colon = ep.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= ep.size()) {
        return false;
    }
    host = ep.substr(0, colon);
    long p = std::strtol(ep.c_str() + colon + 1, nullptr, 10);
    port = static_cast<uint16_t>(p);
    return true;
}

DistributedClient::DistributedClient(size_t replica_size,
                                     std::vector<std::string> servers, int qr,
                                     int qw, int timeout_ms, uint32_t seed)
    : replica_size_(replica_size), hasher_(servers, seed) {
    if (servers.size() < replica_size_) {
        throw std::invalid_argument(
            "need at least R servers to form a replica set");
    }

    const int R = static_cast<int>(replica_size_);
    if (qw <= 0) qw = R / 2 + 1;
    if (qr <= 0) qr = R - qw + 1;
    qr_ = qr;
    qw_ = qw;

    if (qr_ > R || qw_ > R || qr_ + qw_ <= R || 2 * qw_ <= R) {
        throw std::invalid_argument(
            "invalid quorum: require Qr<=R, Qw<=R, Qr+Qw>R, 2*Qw>R");
    }

    for (const std::string& ep : servers) {
        auto c = std::make_unique<Client>();
        std::string host;
        uint16_t port = 0;
        if (parse_endpoint(ep, host, port)) {
            if (c->connect(host, port)) {
                c->set_timeout_ms(timeout_ms);
            }
        }
        clients_.push_back(std::move(c));
    }
}

int DistributedClient::reachable() const {
    int n = 0;
    for (const auto& c : clients_) {
        if (c->connected()) ++n;
    }
    return n;
}

std::vector<size_t> DistributedClient::replicas_for(
    const std::string& key) const {
    return hasher_.replica_set(key, replica_size_);
}

DistributedClient::GetResult DistributedClient::get(const std::string& key) {
    GetResult result;

    std::map<std::string, int> votes;
    const std::string kAbsent = "\x01NULL";

    std::vector<size_t> replicas = replicas_for(key);
    for (size_t idx : replicas) {
        Client* c = clients_[idx].get();
        if (!c->connected()) continue;

        std::string resp = c->get(key);
        if (resp.empty()) continue;
        ++result.responses;

        std::string bucket;
        if (resp.rfind("VALUE ", 0) == 0) {
            bucket = "V" + resp.substr(6);
        } else if (resp == "NULL") {
            bucket = kAbsent;
        } else {
            continue;
        }

        int count = ++votes[bucket];
        if (count >= qr_) {
            result.agree = count;
            if (bucket == kAbsent) {
                result.status = GetResult::NOT_FOUND;
            } else {
                result.status = GetResult::OK;
                result.value = bucket.substr(1);
            }
            return result;
        }
    }

    int best = 0;
    for (const auto& kv : votes) best = std::max(best, kv.second);
    result.agree = best;
    result.status = GetResult::INCONSISTENT;
    return result;
}

DistributedClient::PutResult DistributedClient::put(const std::string& key,
                                                    const std::string& value) {
    PutResult result;
    std::vector<size_t> replicas = replicas_for(key);

    std::vector<size_t> prepared;
    for (size_t idx : replicas) {
        Client* c = clients_[idx].get();
        if (!c->connected()) continue;

        std::string b = c->begin({key});
        if (b.rfind("OK", 0) != 0) continue;

        std::string p = c->put(key, value);
        if (p.rfind("OK", 0) != 0) {
            c->abort();
            continue;
        }
        prepared.push_back(idx);
    }
    result.prepared = static_cast<int>(prepared.size());

    if (result.prepared < qw_) {
        for (size_t idx : prepared) clients_[idx]->abort();
        result.status = PutResult::ABORTED_CONSISTENT;
        return result;
    }

    for (size_t idx : prepared) {
        std::string cm = clients_[idx]->commit();
        if (cm.rfind("OK", 0) == 0) ++result.committed;
    }

    if (result.committed >= qw_) {
        result.status = PutResult::COMMITTED;
    } else {
        result.status = PutResult::INCONSISTENT;
    }
    return result;
}