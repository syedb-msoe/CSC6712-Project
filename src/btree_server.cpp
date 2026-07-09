#include "btree_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <stdexcept>

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool valid_token(const std::string& s) {
    return !s.empty() && s.size() <= protocol::MAX_TOKEN;
}

BTreeServer::BTreeServer(const std::string& db_path)
    : db_(db_path) {
    if (pipe(pipe_fds_) != 0)
        throw std::runtime_error("Failed to create wake pipe");
    set_nonblocking(pipe_fds_[0]);
    set_nonblocking(pipe_fds_[1]);
}

BTreeServer::~BTreeServer() {
    for (auto& kv : conns_) {
        close(kv.second->fd);
    }
    if (listen_fd_ >= 0) close(listen_fd_);
    close(pipe_fds_[0]);
    close(pipe_fds_[1]);
}

void BTreeServer::request_shutdown() {
    shutting_down_ = true;
}

void BTreeServer::log(const std::string& msg) const {
    if (!verbose_) return;
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    std::cerr << "[" << ts << "] " << msg << std::endl;
}

bool BTreeServer::listen_on(uint16_t port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        bound_port_ = ntohs(addr.sin_port);

    set_nonblocking(listen_fd_);
    log("Listening on port " + std::to_string(bound_port_));
    return true;
}

void BTreeServer::run() {
    while (true) {
        if (shutting_down_) {
            if (listen_fd_ >= 0) {
                close(listen_fd_);
                listen_fd_ = -1;
            }
            for (auto& kv : conns_) kv.second->draining = true;
            if (conns_.empty()) break;
        }

        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        int maxfd = -1;
        bool has_pending = false;

        if (listen_fd_ >= 0) {
            FD_SET(listen_fd_, &rset);
            maxfd = std::max(maxfd, listen_fd_);
        }
        FD_SET(pipe_fds_[0], &rset);
        maxfd = std::max(maxfd, pipe_fds_[0]);
        for (auto& kv : conns_) {
            Connection& c = *kv.second;
            if (!c.draining) {
                FD_SET(c.fd, &rset);
                maxfd = std::max(maxfd, c.fd);
            }
            if (!c.out_buf.empty()) {
                FD_SET(c.fd, &wset);
                maxfd = std::max(maxfd, c.fd);
            }
            if (c.in_buf.find('\n') != std::string::npos) has_pending = true;
        }

        timeval zero{0, 0};
        timeval* tv = has_pending ? &zero : nullptr;
        int ready = select(maxfd + 1, &rset, &wset, nullptr, tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(pipe_fds_[0], &rset)) {
            char drain[64];
            while (read(pipe_fds_[0], drain, sizeof(drain)) > 0) {
            }
        }

        if (listen_fd_ >= 0 && FD_ISSET(listen_fd_, &rset)) accept_new();

        std::vector<int> fds;
        fds.reserve(conns_.size());
        for (auto& kv : conns_) fds.push_back(kv.first);

        // Note: Iterating in 4 seperate loops below so that its round-robin-ish and doesn't favor any particular connection.

        // Read connections
        for (int fd : fds) {
            auto it = conns_.find(fd);
            if (FD_ISSET(fd, &rset)) handle_readable(*it->second);
        }

        // Process commands
        for (int fd : fds) {
            auto it = conns_.find(fd);
            process_one_command(*it->second);
        }

        // Write connections
        for (int fd : fds) {
            auto it = conns_.find(fd);
            Connection& c = *it->second;
            if (!c.out_buf.empty() && (FD_ISSET(fd, &wset) || c.draining))
                flush_writable(c);
        }

        // Close connections that are done
        for (int fd : fds) {
            auto it = conns_.find(fd);
            Connection& c = *it->second;
            bool idle = c.out_buf.empty() && c.in_buf.find('\n') == std::string::npos;
            if (c.should_close || (c.draining && idle)) close_connection(fd);
        }
    }
    log("Server stopped.");
}

void BTreeServer::accept_new() {
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) break;
        set_nonblocking(fd);
        auto c = std::make_unique<Connection>();
        c->fd = fd;
        conns_[fd] = std::move(c);
        log("Accepted connection fd=" + std::to_string(fd));
    }
}

void BTreeServer::handle_readable(Connection& c) {
    char buf[4096];
    ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
    if (n == 0) {
        c.should_close = true;
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        c.should_close = true;
        return;
    }
    c.in_buf.append(buf, static_cast<size_t>(n));
    if (c.in_buf.find('\n') == std::string::npos) {
        c.out_buf += "ERR PROTOCOL\n";
        c.in_buf.clear();
        c.draining = true;
    }
}

bool BTreeServer::process_one_command(Connection& c) {
    size_t nl = c.in_buf.find('\n');
    if (nl == std::string::npos) return false;

    std::string line = c.in_buf.substr(0, nl);
    c.in_buf.erase(0, nl + 1);

    protocol::Request req = protocol::parse(line);
    std::string resp = handle_request(c, req);
    c.out_buf += resp;
    c.out_buf += '\n';
    return true;
}

void BTreeServer::flush_writable(Connection& c) {
    while (!c.out_buf.empty()) {
        ssize_t n = send(c.fd, c.out_buf.data(), c.out_buf.size(), 0);
        if (n > 0) {
            c.out_buf.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            c.should_close = true;
            break;
        }
    }
}

void BTreeServer::close_connection(int fd) {
    auto it = conns_.find(fd);
    close(fd);
    log("Closed connection fd=" + std::to_string(fd));
    conns_.erase(it);
}

std::string BTreeServer::handle_request(Connection& c, const protocol::Request& req) {
    switch (req.command) {
        case protocol::Command::PUT:      return do_put(c, req);
        case protocol::Command::GET:      return do_get(c, req);
        case protocol::Command::CONTAINS: return do_contains(c, req);
        case protocol::Command::UNKNOWN:
        default:
            return "ERR PROTOCOL";
    }
}

std::string BTreeServer::do_put(Connection& c, const protocol::Request& req) {
    (void)c;
    if (req.args.size() != 2) return "ERR PROTOCOL";
    const std::string& key = req.args[0];
    const std::string& value = req.args[1];
    if (!valid_token(key) || !valid_token(value)) return "ERR PROTOCOL";

    auto old = db_put(key, value);
    return old ? "OK " + *old : "OK NULL";
}

std::string BTreeServer::do_get(Connection& c, const protocol::Request& req) {
    (void)c;
    if (req.args.size() != 1) return "ERR PROTOCOL";
    const std::string& key = req.args[0];
    if (!valid_token(key)) return "ERR PROTOCOL";

    auto disk = db_get(key);
    return disk ? "VALUE " + *disk : "NULL";
}

std::string BTreeServer::do_contains(Connection& c, const protocol::Request& req) {
    (void)c;
    if (req.args.size() != 1) return "ERR PROTOCOL";
    const std::string& key = req.args[0];
    if (!valid_token(key)) return "ERR PROTOCOL";

    return db_contains(key) ? "TRUE" : "FALSE";
}

bool BTreeServer::db_contains(const std::string& key) {
    char k[KEY_SIZE];
    protocol::to_fixed(key, k, KEY_SIZE);
    return db_.contains(k);
}

std::optional<std::string> BTreeServer::db_get(const std::string& key) {
    char k[KEY_SIZE];
    protocol::to_fixed(key, k, KEY_SIZE);
    auto v = db_.get(k);
    if (!v) return std::nullopt;
    return protocol::from_fixed(*v);
}

std::optional<std::string> BTreeServer::db_put(const std::string& key,
                                               const std::string& value) {
    char k[KEY_SIZE], v[VALUE_SIZE];
    protocol::to_fixed(key, k, KEY_SIZE);
    protocol::to_fixed(value, v, VALUE_SIZE);
    auto old = db_.put(k, v);
    if (!old) return std::nullopt;
    return protocol::from_fixed(*old);
}