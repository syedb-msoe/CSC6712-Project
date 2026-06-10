#include "btree.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>


// Create database file with empty tree
// O_CREAT = create if not exists, O_RDWR = read/write, O_TRUNC = truncate to 0 length
bool BTreeDB::create(const std::string& path) {
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return false;

    HeaderPage h{};
    h.root_index = 0;
    if (write(fd, &h, PAGE_SIZE) != PAGE_SIZE) {
        close(fd);
        return false;
    }
    fsync(fd);
    close(fd);
    return true;
}

BTreeDB::BTreeDB(const std::string& path) {
    fd_ = open(path.c_str(), O_RDWR);
    if (fd_ < 0)
        throw std::runtime_error("Cannot open database file: " + path);
}

BTreeDB::~BTreeDB() {
    if (fd_ >= 0) close(fd_);
}
