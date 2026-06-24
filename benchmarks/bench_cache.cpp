#include "btree.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static const std::string DB_PREFIX = "../database_storage/bench_cache_";
static const int NUM_KEYS = 100000;
static const int READS_PER_TRIAL = 10;

static void make_key(char buf[KEY_SIZE], uint64_t id) {
    std::memset(buf, 0, KEY_SIZE);
    std::snprintf(buf, KEY_SIZE, "key_%020lu", id);
}

static void make_value(char buf[VALUE_SIZE], uint64_t id) {
    std::memset(buf, 0, VALUE_SIZE);
    std::snprintf(buf, VALUE_SIZE, "val_%020lu", id);
}

static std::string db_path(int trial) {
    return DB_PREFIX + std::to_string(trial) + ".db";
}

static void setup(int num_trials) {
    std::cerr << "Creating " << num_trials << " database files with "
              << NUM_KEYS << " keys each..." << std::endl;

    for (int t = 0; t < num_trials; t++) {
        std::string path = db_path(t);
        std::remove(path.c_str());
        BTreeDB::create(path);
        BTreeDB db(path);

        char key[KEY_SIZE], value[VALUE_SIZE];
        for (int i = 0; i < NUM_KEYS; i++) {
            make_key(key, i);
            make_value(value, i);
            db.put(key, value);
        }
        std::cerr << "  Created " << path << std::endl;
    }
    std::cerr << "Done. Now reboot the machine to clear the cache, then run with 'read' mode." << std::endl;
}

static void read_bench(int num_trials) {
    std::cout << "trial,sequence_num,elapsed_seconds" << std::endl;

    for (int t = 0; t < num_trials; t++) {
        std::string path = db_path(t);
        BTreeDB db(path);

        std::mt19937 rng(t);
        std::uniform_int_distribution<int> dist(0, NUM_KEYS - 1);

        for (int seq = 1; seq <= READS_PER_TRIAL; seq++) {
            int key_id = dist(rng);
            char key[KEY_SIZE];
            make_key(key, key_id);

            auto start = std::chrono::high_resolution_clock::now();
            db.get(key);
            auto end = std::chrono::high_resolution_clock::now();

            std::chrono::duration<double> elapsed = end - start;
            std::cout << t << "," << seq << "," << elapsed.count() << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <setup|read> <num_trials>" << std::endl;
        return 1;
    }

    std::string mode = argv[1];
    int num_trials = std::atoi(argv[2]);

    if (num_trials <= 0) {
        std::cerr << "num_trials must be a positive integer" << std::endl;
        return 1;
    }

    if (mode == "setup") {
        setup(num_trials);
    } else if (mode == "read") {
        read_bench(num_trials);
    } else {
        std::cerr << "Unknown mode: " << mode << ". Use 'setup' or 'read'." << std::endl;
        return 1;
    }

    return 0;
}