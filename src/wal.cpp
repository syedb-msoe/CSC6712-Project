#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "btree.h"
#include "protocol.h"

Wal::Wal(const std::string& path) : path_(path) {
    fd_ = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd_ < 0)
        throw std::runtime_error("Cannot open WAL file: " + path);
}

Wal::~Wal() {
    if (fd_ >= 0) close(fd_);
}

void Wal::append_line(const std::string& line, bool sync) {
    std::string buf = line;
    buf.push_back('\n');
    ssize_t written = write(fd_, buf.data(), buf.size());
    if (written != static_cast<ssize_t>(buf.size()))
        throw std::runtime_error("Failed to write WAL record");
    if (sync) fsync(fd_);
}

void Wal::log_begin(uint64_t txn_id, const std::vector<std::string>& keys) {
    std::ostringstream os;
    os << "BEGIN " << txn_id;
    for (const std::string& k : keys) os << ' ' << k;
    append_line(os.str(), false);
}

void Wal::log_write(uint64_t txn_id, const std::string& key, const std::string& value) {
    std::ostringstream os;
    os << "WRITE " << txn_id << ' ' << key << ' ' << value;
    append_line(os.str(), false);
}

void Wal::log_commit(uint64_t txn_id) {
    std::ostringstream os;
    os << "COMMIT " << txn_id;
    append_line(os.str(), true);
}

void Wal::log_abort(uint64_t txn_id) {
    std::ostringstream os;
    os << "ABORT " << txn_id;
    append_line(os.str(), false);
}

void Wal::checkpoint() {
    if (ftruncate(fd_, 0) != 0)
        throw std::runtime_error("Failed to checkpoint (truncate) WAL");
    lseek(fd_, 0, SEEK_SET);
    fsync(fd_);
}

uint64_t Wal::recover(const std::string& path, BTreeDB& db) {
    std::ifstream in(path);
    if (!in.is_open()) return 0;

    std::unordered_set<uint64_t> committed;
    std::vector<std::pair<uint64_t, std::pair<std::string, std::string>>> writes;
    uint64_t max_txn = 0;

    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> tok = protocol::tokenize(line);
        if (tok.empty()) continue;
        const std::string& type = tok[0];

        if (type == "BEGIN" && tok.size() >= 2) {
            uint64_t id = std::strtoull(tok[1].c_str(), nullptr, 10);
            max_txn = std::max(max_txn, id);
        } else if (type == "WRITE" && tok.size() >= 4) {
            uint64_t id = std::strtoull(tok[1].c_str(), nullptr, 10);
            max_txn = std::max(max_txn, id);
            writes.push_back({id, {tok[2], tok[3]}});
        } else if (type == "COMMIT" && tok.size() >= 2) {
            uint64_t id = std::strtoull(tok[1].c_str(), nullptr, 10);
            max_txn = std::max(max_txn, id);
            committed.insert(id);
        } else if (type == "ABORT" && tok.size() >= 2) {
            uint64_t id = std::strtoull(tok[1].c_str(), nullptr, 10);
            max_txn = std::max(max_txn, id);
        }
    }

    for (const auto& w : writes) {
        if (committed.count(w.first) == 0) continue;
        char key[KEY_SIZE], value[VALUE_SIZE];
        protocol::to_fixed(w.second.first, key, KEY_SIZE);
        protocol::to_fixed(w.second.second, value, VALUE_SIZE);
        db.put(key, value);
    }

    return max_txn;
}
