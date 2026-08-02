#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client.h"
#include "rendezvous.h"

class DistributedClient {
public:
    struct GetResult {
        enum Status {
            OK,
            NOT_FOUND,
            INCONSISTENT,
        };
        Status status = INCONSISTENT;
        std::string value;
        int agree = 0;
        int responses = 0;
    };

    struct PutResult {
        enum Status {
            COMMITTED,
            ABORTED_CONSISTENT,
            INCONSISTENT,
        };
        Status status = ABORTED_CONSISTENT;
        int prepared = 0;
        int committed = 0;
    };

    DistributedClient(size_t replica_size, std::vector<std::string> servers,
                      int qr = 0, int qw = 0, int timeout_ms = 2000,
                      uint32_t seed = 0);

    size_t replica_size() const { return replica_size_; }
    int read_quorum() const { return qr_; }
    int write_quorum() const { return qw_; }

    int reachable() const;

    GetResult get(const std::string& key);
    PutResult put(const std::string& key, const std::string& value);

private:
    size_t replica_size_;
    int qr_;
    int qw_;
    RendezvousHasher hasher_;
    std::vector<std::unique_ptr<Client>> clients_;

    std::vector<size_t> replicas_for(const std::string& key) const;
};