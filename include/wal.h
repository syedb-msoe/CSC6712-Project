#pragma once

#include <cstdint>
#include <string>
#include <vector>

class BTreeDB;

class Wal {
public:

    explicit Wal(const std::string& path);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    void log_begin(uint64_t txn_id, const std::vector<std::string>& keys);
    void log_write(uint64_t txn_id, const std::string& key, const std::string& value);
    void log_commit(uint64_t txn_id);
    void log_abort(uint64_t txn_id);

    void checkpoint();

    static uint64_t recover(const std::string& path, BTreeDB& db);

private:
    int fd_;
    std::string path_;

    void append_line(const std::string& line, bool sync);
};
