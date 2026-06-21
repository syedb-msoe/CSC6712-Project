#include <gtest/gtest.h>
#include "btree.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

static const std::string TEST_DB = "/tmp/btree_test.db";

static void make_key(char buf[KEY_SIZE], const std::string& s) {
    std::memset(buf, 0, KEY_SIZE);
    std::memcpy(buf, s.c_str(), std::min(s.size(), (size_t)KEY_SIZE));
}

static void make_value(char buf[VALUE_SIZE], const std::string& s) {
    std::memset(buf, 0, VALUE_SIZE);
    std::memcpy(buf, s.c_str(), std::min(s.size(), (size_t)VALUE_SIZE));
}

static std::string trim_value(const std::string& v) {
    // Remove trailing null bytes
    auto pos = v.find('\0');
    return pos == std::string::npos ? v : v.substr(0, pos);
}

class BTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::remove(TEST_DB.c_str());
        BTreeDB::create(TEST_DB);
    }
    void TearDown() override {
        std::remove(TEST_DB.c_str());
    }
};

TEST_F(BTreeTest, CreateEmpty) {
    BTreeDB db(TEST_DB);
    char key[KEY_SIZE];
    make_key(key, "hello");
    EXPECT_FALSE(db.contains(key));
    EXPECT_EQ(db.get(key), std::nullopt);
}

TEST_F(BTreeTest, PutAndGet) {
    BTreeDB db(TEST_DB);
    char k[KEY_SIZE], v[VALUE_SIZE];
    make_key(k, "key1");
    make_value(v, "value1");

    auto old = db.put(k, v);
    EXPECT_EQ(old, std::nullopt);
    EXPECT_TRUE(db.contains(k));

    auto result = db.get(k);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(trim_value(*result), "value1");
}