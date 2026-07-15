#include <getopt.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>

#include "btree.h"

static uint64_t random_number(uint64_t seed) {
    std::mt19937_64 rng(seed);
    return rng();
}

static void make_key(char buf[KEY_SIZE], uint64_t id) {
    std::memset(buf, 0, KEY_SIZE);
    std::snprintf(buf, KEY_SIZE, "key_%020lu", random_number(2 * id));
}

static void make_value(char buf[VALUE_SIZE], uint64_t id) {
    std::memset(buf, 0, VALUE_SIZE);
    std::snprintf(buf, VALUE_SIZE, "val_%020lu", random_number(2 * id + 1));
}

int main(int argc, char* argv[]) {
    std::string db_path = "bench.db";
    long count = 1000000;
    bool verbose = false;

    static struct option long_opts[] = {
        {"db", required_argument, nullptr, 'd'},
        {"count", required_argument, nullptr, 'n'},
        {"verbose", no_argument, nullptr, 'v'},
        {nullptr, 0, nullptr, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "d:n:v", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'd': db_path = optarg; break;
            case 'n': count = std::atol(optarg); break;
            case 'v': verbose = true; break;
            default: break;
        }
    }

    std::remove(db_path.c_str());
    if (!BTreeDB::create(db_path)) {
        std::cerr << "Failed to create database file: " << db_path << std::endl;
        return 1;
    }

    BTreeDB db(db_path);
    char key[KEY_SIZE], value[VALUE_SIZE];
    for (long i = 0; i < count; i++) {
        make_key(key, i);
        make_value(value, i);
        db.put(key, value);

        if (verbose && count >= 100000 && i % 100000 == 0)
            std::cerr << "loaded " << i << " / " << count << std::endl;
    }

    std::cerr << "Loaded " << count << " key-value pairs into " << db_path << std::endl;
    return 0;
}
