#include "btree.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <cstring>
#include <algorithm>


// Create database file with empty tree
// (https://man7.org/linux/man-pages/man2/open.2.html)
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

HeaderPage BTreeDB::read_header() {
    HeaderPage h{};
    lseek(fd_, 0, SEEK_SET);
    if (read(fd_, &h, PAGE_SIZE) != PAGE_SIZE)
        throw std::runtime_error("Failed to read header");
    return h;
}

void BTreeDB::write_header(const HeaderPage& h) {
    lseek(fd_, 0, SEEK_SET);
    if (write(fd_, &h, PAGE_SIZE) != PAGE_SIZE)
        throw std::runtime_error("Failed to write header");
}

NodePage BTreeDB::read_node(uint32_t page_index) {
    NodePage np{};
    off_t offset = static_cast<off_t>(page_index) * PAGE_SIZE;
    lseek(fd_, offset, SEEK_SET);
    if (read(fd_, &np, PAGE_SIZE) != PAGE_SIZE)
        throw std::runtime_error("Failed to read node");
    return np;
}

void BTreeDB::write_node(uint32_t page_index, const NodePage& np) {
    NodePage np{};
    off_t offset = static_cast<off_t>(page_index) * PAGE_SIZE;
    lseek(fd_, offset, SEEK_SET);
    if (write(fd_, &np, PAGE_SIZE) != PAGE_SIZE)
        throw std::runtime_error("Failed to write node");
}

void BTreeDB::sync() {
    fsync(fd_);
}

uint32_t BTreeDB::allocate_node() {
    off_t end = lseek(fd_, 0, SEEK_END);
    uint32_t page_index = static_cast<uint32_t>(end / PAGE_SIZE);
    //TODO: allocate memory for node when reaching insertion logic
    return page_index;
}

BTreeDB::SearchResult BTreeDB::search(uint32_t root_index, const char key[KEY_SIZE]) {
    if (root_index == 0) return {0, -1};

    uint32_t cur = root_index;
    while (cur != 0) {
        NodePage np = read_node(cur);
        const BTreeNode& n = np.node;

        int i = 0;
        while (i < static_cast<int>(n.num_keys) && std::memcmp(key, n.keys[i], KEY_SIZE) > 0)
            i++;

        if (i < static_cast<int>(n.num_keys) && std::memcmp(key, n.keys[i], KEY_SIZE) == 0)
            return {cur, i};

        if (n.children[0] == 0)
            return {0, -1};

        cur = n.children[i];
    }
    return {0, -1};
}

bool BTreeDB::contains(const char key[KEY_SIZE]) {
    HeaderPage h = read_header();
    if (h.root_index == 0) return false;
    auto r = search(h.root_index, key);
    return r.key_index >= 0;
}

std::optional<std::string> BTreeDB::get(const char key[KEY_SIZE]) {
    HeaderPage h = read_header();
    if (h.root_index == 0) return std::nullopt;
    auto r = search(h.root_index, key);
    if (r.key_index < 0) return std::nullopt;

    NodePage np = read_node(r.page_index);
    return std::string(np.node.values[r.key_index], VALUE_SIZE);
}

void BTreeDB::split_child(uint32_t parent_index, int child_idx) {

}

void BTreeDB::insert_nonfull(uint32_t node_index, const char key[KEY_SIZE], const char value[VALUE_SIZE]) {

}

std::optional<std::string> BTreeDB::put(const char key[KEY_SIZE], const char value[VALUE_SIZE]) {
    return std::nullopt;
}