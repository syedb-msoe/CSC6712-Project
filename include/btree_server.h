#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "btree.h"
#include "protocol.h"
#include "wal.h"

class BTreeServer {
public:
    BTreeServer(const std::string& db_path, const std::string& wal_path);
    ~BTreeServer();

    BTreeServer(const BTreeServer&) = delete;
    BTreeServer& operator=(const BTreeServer&) = delete;

    void set_verbose(bool v) { verbose_ = v; }

    void set_debug(bool d) { debug_ = d; }

    void set_txn_timeout_ms(long ms) { txn_timeout_ms_ = ms; }

    bool listen_on(uint16_t port);

    uint16_t bound_port() const { return bound_port_; }

    void run();

    void request_shutdown();

private:
    struct Connection {
        int fd = -1;
        std::string in_buf;
        std::string out_buf;
        bool draining = false;
        bool should_close = false;
        bool in_txn = false;
        uint64_t txn_id = 0;
        std::chrono::system_clock::time_point expires_at;
        std::vector<std::string> locked_keys;
        std::unordered_map<std::string, std::string> write_buf;
    };

    int listen_fd_ = -1;
    uint16_t bound_port_ = 0;
    int pipe_fds_[2] = {-1, -1};
    std::unordered_map<int, std::unique_ptr<Connection>> conns_;

    BTreeDB db_;
    Wal wal_;
    uint64_t next_txn_id_ = 1;
    std::unordered_map<std::string, int> locks_;
    int active_txn_count_ = 0;

    bool shutting_down_ = false;
    bool verbose_ = false;
    bool debug_ = false;
    long txn_timeout_ms_ = protocol::TXN_TIMEOUT_MS;

    void accept_new();
    void handle_readable(Connection& c);
    void flush_writable(Connection& c);
    bool process_one_command(Connection& c);
    void close_connection(int fd);

    std::string handle_request(Connection& c, const protocol::Request& req);
    std::string do_put(Connection& c, const protocol::Request& req);
    std::string do_get(Connection& c, const protocol::Request& req);
    std::string do_contains(Connection& c, const protocol::Request& req);
    std::string do_begin(Connection& c, const protocol::Request& req);
    std::string do_commit(Connection& c);
    std::string do_abort(Connection& c);
    bool key_locked_by_other(const std::string& key, int fd) const;
    void release_locks(Connection& c);
    bool txn_expired(const Connection& c) const;
    void end_transaction(Connection& c);

    bool db_contains(const std::string& key);
    std::optional<std::string> db_get(const std::string& key);
    std::optional<std::string> db_put(const std::string& key, const std::string& value);

    void log(const std::string& msg) const;
    void debug_pause(const std::string& point);
};