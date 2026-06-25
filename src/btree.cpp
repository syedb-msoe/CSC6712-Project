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

    NodePage np{};
    np.node.parent_index = 0;
    np.node.num_keys = 0;
    std::memset(np.node.children, 0, sizeof(np.node.children));
    if (write(fd_, &np, PAGE_SIZE) != PAGE_SIZE)
        throw std::runtime_error("Failed to allocate node");
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
    BTreeDB::SearchResult r = search(h.root_index, key);
    return r.key_index >= 0;
}

std::optional<std::string> BTreeDB::get(const char key[KEY_SIZE]) {
    HeaderPage h = read_header();
    if (h.root_index == 0) return std::nullopt;
    BTreeDB::SearchResult r = search(h.root_index, key);
    if (r.key_index < 0) return std::nullopt;

    NodePage np = read_node(r.page_index);
    return std::string(np.node.values[r.key_index], VALUE_SIZE);
}

void BTreeDB::split_child(uint32_t parent_index, int child_idx) {
    NodePage parent_np = read_node(parent_index);
    BTreeNode& parent = parent_np.node;

    uint32_t full_child_index = parent.children[child_idx];
    NodePage full_np = read_node(full_child_index);
    BTreeNode& full = full_np.node;

    int mid_node_index = ((MAX_KEYS + 1) / 2) - 1;

    uint32_t new_index = allocate_node();
    NodePage new_np{};
    BTreeNode& nn = new_np.node;
    nn.parent_index = parent_index;
    nn.num_keys = full.num_keys - mid_node_index - 1;

    for (uint32_t j = 0; j < nn.num_keys; j++) {
        std::memcpy(nn.keys[j], full.keys[mid_node_index + 1 + j], KEY_SIZE);
        std::memcpy(nn.values[j], full.values[mid_node_index + 1 + j], VALUE_SIZE);
    }

    if (full.children[0] != 0) {
        for (uint32_t j = 0; j <= nn.num_keys; j++) {
            nn.children[j] = full.children[mid_node_index + 1 + j];
            if (nn.children[j] != 0) {
                NodePage child_np = read_node(nn.children[j]);
                child_np.node.parent_index = new_index;
                write_node(nn.children[j], child_np);
            }
        }
    }

    for (int j = static_cast<int>(parent.num_keys); j > child_idx; j--) {
        std::memcpy(parent.keys[j], parent.keys[j - 1], KEY_SIZE);
        std::memcpy(parent.values[j], parent.values[j - 1], VALUE_SIZE);
        parent.children[j + 1] = parent.children[j];
    }

    std::memcpy(parent.keys[child_idx], full.keys[mid_node_index], KEY_SIZE);
    std::memcpy(parent.values[child_idx], full.values[mid_node_index], VALUE_SIZE);
    parent.children[child_idx + 1] = new_index;
    parent.num_keys++;

    full.num_keys = mid_node_index;

    write_node(parent_index, parent_np);
    write_node(full_child_index, full_np);
    write_node(new_index, new_np);
}

void BTreeDB::insert_nonfull(uint32_t node_index, const char key[KEY_SIZE], const char value[VALUE_SIZE]) {
    NodePage np = read_node(node_index);
    BTreeNode& n = np.node;

    int i = static_cast<int>(n.num_keys) - 1;

    if (n.children[0] == 0) {
        while (i >= 0 && std::memcmp(key, n.keys[i], KEY_SIZE) < 0) {
            std::memcpy(n.keys[i + 1], n.keys[i], KEY_SIZE);
            std::memcpy(n.values[i + 1], n.values[i], VALUE_SIZE);
            i--;
        }
        std::memcpy(n.keys[i + 1], key, KEY_SIZE);
        std::memcpy(n.values[i + 1], value, VALUE_SIZE);
        n.num_keys++;
        write_node(node_index, np);
    } else {
        while (i >= 0 && std::memcmp(key, n.keys[i], KEY_SIZE) < 0)
            i--;
        i++;

        NodePage child_np = read_node(n.children[i]);
        if (child_np.node.num_keys == MAX_KEYS) {
            split_child(node_index, i);
            np = read_node(node_index);
            if (std::memcmp(key, np.node.keys[i], KEY_SIZE) > 0)
                i++;
        }
        np = read_node(node_index);
        insert_nonfull(np.node.children[i], key, value);
    }
}

std::optional<std::string> BTreeDB::put(const char key[KEY_SIZE], const char value[VALUE_SIZE]) {
    HeaderPage h = read_header();

    if (h.root_index != 0) {
        auto r = search(h.root_index, key);
        if (r.key_index >= 0) {
            NodePage np = read_node(r.page_index);
            std::string old_val(np.node.values[r.key_index], VALUE_SIZE);
            std::memcpy(np.node.values[r.key_index], value, VALUE_SIZE);
            write_node(r.page_index, np);
            sync();
            return old_val;
        }
    }

    if (h.root_index == 0) {
        uint32_t root = allocate_node();
        NodePage np{};
        np.node.num_keys = 1;
        np.node.parent_index = 0;
        std::memcpy(np.node.keys[0], key, KEY_SIZE);
        std::memcpy(np.node.values[0], value, VALUE_SIZE);
        write_node(root, np);

        h.root_index = root;
        write_header(h);
        sync();
        return std::nullopt;
    }

    NodePage root_np = read_node(h.root_index);
    if (root_np.node.num_keys == MAX_KEYS) {
        uint32_t new_root = allocate_node();
        NodePage new_root_np{};
        new_root_np.node.num_keys = 0;
        new_root_np.node.parent_index = 0;
        new_root_np.node.children[0] = h.root_index;
        write_node(new_root, new_root_np);

        root_np.node.parent_index = new_root;
        write_node(h.root_index, root_np);

        split_child(new_root, 0);

        h.root_index = new_root;
        write_header(h);

        insert_nonfull(new_root, key, value);
    } else {
        insert_nonfull(h.root_index, key, value);
    }

    sync();
    return std::nullopt;
}