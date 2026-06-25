#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

static constexpr uint32_t PAGE_SIZE = 4096;
static constexpr uint32_t KEY_SIZE = 64;
static constexpr uint32_t VALUE_SIZE = 64;

struct HeaderPage {
    uint32_t root_index;
    char padding[PAGE_SIZE - sizeof(uint32_t)];
};

// Calculate max keys per node:
// 64 bytes per key + 64 bytes per value
// Additional metadata needed: parent index (4 bytes), num_keys (4 bytes), children pointers (4 bytes each)
// 8 + N(128) + (N+1)(4) <= 4096 Where N is number of key-value pairs
// Solving gives N = 30, so max 30 keys per node
static constexpr uint32_t MAX_KEYS = 30;

struct BTreeNode {
    uint32_t parent_index;
    uint32_t num_keys;
    char keys[MAX_KEYS][KEY_SIZE];
    char values[MAX_KEYS][VALUE_SIZE];
    uint32_t children[MAX_KEYS + 1];
};

// Padded node for disk I/O
struct NodePage {
    BTreeNode node;
    char padding[PAGE_SIZE - sizeof(BTreeNode)];
};

class BTreeDB {
public:
    // Create or open a database file
    static bool create(const std::string& path);
    explicit BTreeDB(const std::string& path);
    ~BTreeDB();

    BTreeDB(const BTreeDB&) = delete;
    BTreeDB& operator=(const BTreeDB&) = delete;

    // Check if key exists
    bool contains(const char key[KEY_SIZE]);

    // Get value for key. Returns nullopt if not found.
    std::optional<std::string> get(const char key[KEY_SIZE]);

    // Put key-value pair. Returns previous value if key existed.
    std::optional<std::string> put(const char key[KEY_SIZE], const char value[VALUE_SIZE]);

private:
    int fd_;

    HeaderPage read_header();
    void write_header(const HeaderPage& h);
    NodePage read_node(uint32_t page_index);
    void write_node(uint32_t page_index, const NodePage& np);
    void sync();

    // Returns the page index of new node
    uint32_t allocate_node();

    struct SearchResult {
        uint32_t page_index; // 0 if not found
        int key_index; // -1 if not found
    };
    // search logic
    SearchResult search(uint32_t root_index, const char key[KEY_SIZE]);

    // Insert into non-full node
    void insert_nonfull(uint32_t node_index, const char key[KEY_SIZE], const char value[VALUE_SIZE]);

    // Split child at child_idx of parent, required to balance tree
    void split_child(uint32_t parent_index, int child_idx);
};
