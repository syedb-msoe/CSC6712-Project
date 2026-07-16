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

static constexpr size_t MAX_LINE = 8192;

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool valid_token(const std::string& s) {
    return !s.empty() && s.size() <= protocol::MAX_TOKEN;
}

BTreeServer::BTreeServer(const std::string& db_path, const std::string& wal_path)
    : db_(db_path), wal_(wal_path) {
    if (pipe(pipe_fds_) != 0)
        throw std::runtime_error("Failed to create wake pipe");
    set_nonblocking(pipe_fds_[0]);
    set_nonblocking(pipe_fds_[1]);

    uint64_t max_txn = Wal::recover(wal_path, db_);
    next_txn_id_ = max_txn + 1;
    wal_.checkpoint();
    log("Recovery complete; next txn id = " + std::to_string(next_txn_id_));
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

void BTreeServer::debug_pause(const std::string& point) {
    if (!debug_) return;
    std::cerr << "[DEBUG PAUSE] " << point << "\n"
              << "  [enter]=continue  k=kill (simulate crash): " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return;
    if (!line.empty() && (line[0] == 'k' || line[0] == 'K')) {
        std::cerr << "[DEBUG] killing process to simulate a crash" << std::endl;
        _exit(137);
    }
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
            std::vector<int> idle_fds;
            for (auto& kv : conns_) {
                Connection& c = *kv.second;
                c.draining = true;
                if (c.out_buf.empty() && c.in_buf.find('\n') == std::string::npos)
                    idle_fds.push_back(kv.first);
            }
            for (int fd : idle_fds) close_connection(fd);
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

        // Process a single command for each connection
        for (int fd : fds) {
            auto it = conns_.find(fd);
            process_one_command(*it->second);
        }

        // Write connections
        for (int fd : fds) {
            auto it = conns_.find(fd);
            Connection& c = *it->second;
            if (!c.out_buf.empty())
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
    if (c.in_buf.size() > MAX_LINE && c.in_buf.find('\n') == std::string::npos) {
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

    Connection& c = *it->second;
    if (c.in_txn) {
        wal_.log_abort(c.txn_id);
        end_transaction(c);
        if (active_txn_count_ == 0) wal_.checkpoint();
    }
    close(fd);
    log("Closed connection fd=" + std::to_string(fd));
    conns_.erase(it);
}

std::string BTreeServer::handle_request(Connection& c, const protocol::Request& req) {
    switch (req.command) {
        case protocol::Command::PUT:      return do_put(c, req);
        case protocol::Command::GET:      return do_get(c, req);
        case protocol::Command::CONTAINS: return do_contains(c, req);
        case protocol::Command::BEGIN:    return do_begin(c, req);
        case protocol::Command::COMMIT:   return do_commit(c);
        case protocol::Command::ABORT:    return do_abort(c);
        case protocol::Command::SHUTDOWN:
            log("SHUTDOWN requested by fd=" + std::to_string(c.fd));
            shutting_down_ = true;
            return "OK SHUTTING_DOWN";
        case protocol::Command::UNKNOWN:
        default:
            return "ERR PROTOCOL";
    }
}

std::string BTreeServer::do_put(Connection& c, const protocol::Request& req) {
    if (req.args.size() != 2) return "ERR PROTOCOL";
    const std::string& key = req.args[0];
    const std::string& value = req.args[1];
    if (!valid_token(key) || !valid_token(value)) return "ERR PROTOCOL";

    if (c.in_txn) {
        if (txn_expired(c)) return "ERR TXN_EXPIRED";
        if (std::find(c.locked_keys.begin(), c.locked_keys.end(), key) ==
            c.locked_keys.end())
            return "ERR KEY_NOT_LOCKED";

        wal_.log_write(c.txn_id, key, value);

        std::string prev;
        auto bit = c.write_buf.find(key);
        if (bit != c.write_buf.end()) {
            prev = bit->second;
        } else {
            auto disk = db_get(key);
            if (!disk) {
                c.write_buf[key] = value;
                return "OK NULL";
            }
            prev = *disk;
        }
        c.write_buf[key] = value;
        return "OK " + prev;
    }

    reap_expired_lock(key);
    if (key_locked_by_other(key, c.fd)) return "ERR KEY_LOCKED";
    auto old = db_put(key, value);
    return old ? "OK " + *old : "OK NULL";
}

std::string BTreeServer::do_get(Connection& c, const protocol::Request& req) {
    if (req.args.size() != 1) return "ERR PROTOCOL";
    const std::string& key = req.args[0];
    if (!valid_token(key)) return "ERR PROTOCOL";

    if (c.in_txn) {
        if (txn_expired(c)) return "ERR TXN_EXPIRED";
        if (std::find(c.locked_keys.begin(), c.locked_keys.end(), key) ==
            c.locked_keys.end())
            return "ERR KEY_NOT_LOCKED";

        auto bit = c.write_buf.find(key);
        if (bit != c.write_buf.end()) return "VALUE " + bit->second;
        auto disk = db_get(key);
        return disk ? "VALUE " + *disk : "NULL";
    }

    reap_expired_lock(key);
    if (key_locked_by_other(key, c.fd)) return "ERR KEY_LOCKED";
    auto disk = db_get(key);
    return disk ? "VALUE " + *disk : "NULL";
}

std::string BTreeServer::do_contains(Connection& c, const protocol::Request& req) {
    if (req.args.size() != 1) return "ERR PROTOCOL";
    const std::string& key = req.args[0];
    if (!valid_token(key)) return "ERR PROTOCOL";

    if (c.in_txn) {
        if (txn_expired(c)) return "ERR TXN_EXPIRED";
        if (std::find(c.locked_keys.begin(), c.locked_keys.end(), key) ==
            c.locked_keys.end())
            return "ERR KEY_NOT_LOCKED";

        if (c.write_buf.find(key) != c.write_buf.end()) return "TRUE";
        return db_contains(key) ? "TRUE" : "FALSE";
    }

    reap_expired_lock(key);
    if (key_locked_by_other(key, c.fd)) return "ERR KEY_LOCKED";
    return db_contains(key) ? "TRUE" : "FALSE";
}

std::string BTreeServer::do_begin(Connection& c, const protocol::Request& req) {
    if (c.in_txn) return "ERR TXN_ACTIVE";
    if (req.args.empty()) return "ERR PROTOCOL";
    for (const std::string& k : req.args)
        if (!valid_token(k)) return "ERR PROTOCOL";

    for (const std::string& k : req.args) {
        reap_expired_lock(k);
        if (key_locked_by_other(k, c.fd)) return "ERR LOCK_FAILED";
    }

    c.in_txn = true;
    c.txn_id = next_txn_id_++;
    c.locked_keys = req.args;
    c.write_buf.clear();
    c.expires_at = std::chrono::system_clock::now() +
                   std::chrono::milliseconds(txn_timeout_ms_);
    for (const std::string& k : c.locked_keys) locks_[k] = c.fd;
    active_txn_count_++;

    wal_.log_begin(c.txn_id, c.locked_keys);

    long expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          c.expires_at.time_since_epoch())
                          .count();
    log("BEGIN txn=" + std::to_string(c.txn_id) + " fd=" + std::to_string(c.fd));
    return "OK " + std::to_string(expires_ms);
}

std::string BTreeServer::do_commit(Connection& c) {
    if (!c.in_txn) return "ERR NO_TXN";
    if (txn_expired(c)) return "ERR TXN_EXPIRED";
    if (c.write_buf.empty()) return "ERR NO_WRITES";

    wal_.log_commit(c.txn_id);
    debug_pause("COMMIT record written to WAL, before B-tree flush (txn=" +
                std::to_string(c.txn_id) + ")");
    for (const auto& kv : c.write_buf) db_put(kv.first, kv.second);

    uint64_t id = c.txn_id;
    end_transaction(c);
    if (active_txn_count_ == 0) wal_.checkpoint();
    log("COMMIT txn=" + std::to_string(id) + " fd=" + std::to_string(c.fd));
    return "OK";
}

std::string BTreeServer::do_abort(Connection& c) {
    if (!c.in_txn) return "ERR NO_TXN";
    uint64_t id = c.txn_id;
    wal_.log_abort(c.txn_id);
    end_transaction(c);
    if (active_txn_count_ == 0) wal_.checkpoint();
    log("ABORT txn=" + std::to_string(id) + " fd=" + std::to_string(c.fd));
    return "OK";
}

bool BTreeServer::key_locked_by_other(const std::string& key, int fd) const {
    auto it = locks_.find(key);
    return it != locks_.end() && it->second != fd;
}

void BTreeServer::reap_expired_lock(const std::string& key) {
    auto lit = locks_.find(key);
    if (lit == locks_.end()) return;
    auto cit = conns_.find(lit->second);
    if (cit == conns_.end()) return;

    Connection& holder = *cit->second;
    if (!holder.in_txn || !txn_expired(holder)) return;

    uint64_t id = holder.txn_id;
    wal_.log_abort(id);
    end_transaction(holder);
    if (active_txn_count_ == 0) wal_.checkpoint();
    log("Reaped expired txn=" + std::to_string(id) +
        " fd=" + std::to_string(holder.fd));
}

void BTreeServer::release_locks(Connection& c) {
    for (const std::string& k : c.locked_keys) {
        auto it = locks_.find(k);
        if (it != locks_.end() && it->second == c.fd) locks_.erase(it);
    }
    c.locked_keys.clear();
}

bool BTreeServer::txn_expired(const Connection& c) const {
    return std::chrono::system_clock::now() > c.expires_at;
}

void BTreeServer::end_transaction(Connection& c) {
    release_locks(c);
    c.write_buf.clear();
    c.in_txn = false;
    if (active_txn_count_ > 0) active_txn_count_--;
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