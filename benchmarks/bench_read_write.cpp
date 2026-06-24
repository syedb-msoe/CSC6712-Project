#include "btree.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <random>
#include <string>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>

static std::string get_bench_db() {
    std::string home = std::getenv("HOME");
    std::string dir = home + "/bench_data";
    mkdir(dir.c_str(), 0755);
    return dir + "/bench_rw.db";
}

static const std::string BENCH_DB = get_bench_db();

static void make_key(char buf[KEY_SIZE], uint64_t id) {
    std::memset(buf, 0, KEY_SIZE);
    std::snprintf(buf, KEY_SIZE, "key_%020lu", id);
}

static void make_value(char buf[VALUE_SIZE], uint64_t id) {
    std::memset(buf, 0, VALUE_SIZE);
    std::snprintf(buf, VALUE_SIZE, "val_%020lu", id);
}

static double bench_write(int count) {
    std::remove(BENCH_DB.c_str());
    BTreeDB::create(BENCH_DB);
    BTreeDB db(BENCH_DB);

    char key[KEY_SIZE], value[VALUE_SIZE];

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < count; i++) {
        make_key(key, i);
        make_value(value, i);
        db.put(key, value);
    }
    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double> elapsed = end - start;
    return elapsed.count();
}

static double bench_read(int count) {
    std::remove(BENCH_DB.c_str());
    BTreeDB::create(BENCH_DB);
    {
        BTreeDB db(BENCH_DB);
        char key[KEY_SIZE], value[VALUE_SIZE];
        for (int i = 0; i < count; i++) {
            make_key(key, i);
            make_value(value, i);
            db.put(key, value);
        }
    }

    std::vector<int> indices(count);
    for (int i = 0; i < count; i++) indices[i] = i;
    std::mt19937 rng(42);
    std::shuffle(indices.begin(), indices.end(), rng);

    BTreeDB db(BENCH_DB);
    char key[KEY_SIZE];

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < count; i++) {
        make_key(key, indices[i]);
        db.get(key);
    }
    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double> elapsed = end - start;
    return elapsed.count();
}

int main() {
    std::cout << "operation,num_keys,trial,elapsed_seconds" << std::endl;

    std::vector<int> counts = {100, 1000, 10000, 100000, 1000000};
    int num_trials = 3;

    for (int count : counts) {
        for (int trial = 1; trial <= num_trials; trial++) {
            double write_time = bench_write(count);
            std::cout << "write," << count << "," << trial << "," << write_time << std::endl;
        }
        for (int trial = 1; trial <= num_trials; trial++) {
            double read_time = bench_read(count);
            std::cout << "read," << count << "," << trial << "," << read_time << std::endl;
        }
    }

    std::remove(BENCH_DB.c_str());
    return 0;
}