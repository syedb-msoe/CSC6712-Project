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

class BTreeServer {
public:
    BTreeServer(const std::string& db_path);
    ~BTreeServer();

    BTreeServer(const BTreeServer&) = delete;
    BTreeServer& operator=(const BTreeServer&) = delete;

    void set_verbose(bool v) { verbose_ = v; }


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
    };

    int listen_fd_ = -1;
    uint16_t bound_port_ = 0;
    int pipe_fds_[2] = {-1, -1};
    std::unordered_map<int, std::unique_ptr<Connection>> conns_;

    BTreeDB db_;

    bool shutting_down_ = false;
    bool verbose_ = false;

    void accept_new();
    void handle_readable(Connection& c);
    void flush_writable(Connection& c);
    bool process_one_command(Connection& c);
    void close_connection(int fd);

    std::string handle_request(Connection& c, const protocol::Request& req);
    std::string do_put(Connection& c, const protocol::Request& req);
    std::string do_get(Connection& c, const protocol::Request& req);
    std::string do_contains(Connection& c, const protocol::Request& req);

    bool db_contains(const std::string& key);
    std::optional<std::string> db_get(const std::string& key);
    std::optional<std::string> db_put(const std::string& key, const std::string& value);

    void log(const std::string& msg) const;
};