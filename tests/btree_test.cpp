#include <gtest/gtest.h>
#include "btree.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <random>

static const std::string TEST_DB = "../database_storage/btree_test.db";

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

TEST_F(BTreeTest, UpdateExistingKey) {
    BTreeDB db(TEST_DB);
    char k[KEY_SIZE], v1[VALUE_SIZE], v2[VALUE_SIZE];
    make_key(k, "key1");
    make_value(v1, "old_value");
    make_value(v2, "new_value");

    db.put(k, v1);
    auto old = db.put(k, v2);

    ASSERT_TRUE(old.has_value());
    EXPECT_EQ(trim_value(*old), "old_value");

    auto result = db.get(k);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(trim_value(*result), "new_value");
}

TEST_F(BTreeTest, MultipleKeys) {
    BTreeDB db(TEST_DB);
    const int N = 10;
    for (int i = 0; i < N; i++) {
        char k[KEY_SIZE], v[VALUE_SIZE];
        make_key(k, "key" + std::to_string(i));
        make_value(v, "val" + std::to_string(i));
        db.put(k, v);
    }

    for (int i = 0; i < N; i++) {
        char k[KEY_SIZE];
        make_key(k, "key" + std::to_string(i));
        EXPECT_TRUE(db.contains(k));
        auto result = db.get(k);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(trim_value(*result), "val" + std::to_string(i));
    }

    char k[KEY_SIZE];
    make_key(k, "nonexistent");
    EXPECT_FALSE(db.contains(k));
    EXPECT_EQ(db.get(k), std::nullopt);
}

TEST_F(BTreeTest, PersistenceAcrossOpens) {
    {
        BTreeDB db(TEST_DB);
        char k[KEY_SIZE], v[VALUE_SIZE];
        make_key(k, "persist_key");
        make_value(v, "persist_val");
        db.put(k, v);
    }

    {
        BTreeDB db(TEST_DB);
        char k[KEY_SIZE];
        make_key(k, "persist_key");
        EXPECT_TRUE(db.contains(k));
        auto result = db.get(k);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(trim_value(*result), "persist_val");
    }
}

TEST_F(BTreeTest, UpdateSomeKeys) {
    BTreeDB db(TEST_DB);
    const int N = 50;

    for (int i = 0; i < N; i++) {
        char k[KEY_SIZE], v[VALUE_SIZE];
        make_key(k, "u" + std::to_string(i));
        make_value(v, "original" + std::to_string(i));
        db.put(k, v);
    }

    for (int i = 0; i < N; i += 2) {
        char k[KEY_SIZE], v[VALUE_SIZE];
        make_key(k, "u" + std::to_string(i));
        make_value(v, "updated" + std::to_string(i));
        auto old = db.put(k, v);
        ASSERT_TRUE(old.has_value());
        EXPECT_EQ(trim_value(*old), "original" + std::to_string(i));
    }

    for (int i = 0; i < N; i++) {
        char k[KEY_SIZE];
        make_key(k, "u" + std::to_string(i));
        auto result = db.get(k);
        ASSERT_TRUE(result.has_value());
        if (i % 2 == 0)
            EXPECT_EQ(trim_value(*result), "updated" + std::to_string(i));
        else
            EXPECT_EQ(trim_value(*result), "original" + std::to_string(i));
    }
}

TEST_F(BTreeTest, LargeScaleBenchmark) {
    BTreeDB db(TEST_DB);
    const int N = 1000000;

    for (int i = 0; i < N; i++) {
        char k[KEY_SIZE], v[VALUE_SIZE];
        make_key(k, std::string("big") + std::to_string(i));
        make_value(v, std::string("dat") + std::to_string(i));
        db.put(k, v);
    }

    for (int i = 0; i < N; i++) {
        char k[KEY_SIZE];
        make_key(k, std::string("big") + std::to_string(i));
        auto value = db.get(k);
        EXPECT_EQ(trim_value(*value), "dat" + std::to_string(i));
    }
}