#include <getopt.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "client.h"

static std::string make_key(uint64_t id) {
    std::mt19937_64 rng(2 * id);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%020lu", rng());
    return buf;
}

struct Result {
    long requests = 0;
    long errors = 0;
    bool connected = false;
};

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 5555;
    int clients = 1;
    long ops = 10000;
    long num_keys = 1000000;
    int timeout_ms = 20;
    bool verbose = false;

    static struct option long_opts[] = {
        {"host", required_argument, nullptr, 'H'},
        {"port", required_argument, nullptr, 'p'},
        {"clients", required_argument, nullptr, 'c'},
        {"ops", required_argument, nullptr, 'o'},
        {"verbose", no_argument, nullptr, 'v'},
        {nullptr, 0, nullptr, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "H:p:c:o:v", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'H': host = optarg; break;
            case 'p': port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'c': clients = std::atoi(optarg); break;
            case 'o': ops = std::atol(optarg); break;
            case 'v': verbose = true; break;
            default: break;
        }
    }

    std::vector<Result> results(clients);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    auto worker = [&](int id) {
        Client c;
        bool connected = c.connect(host, port);
        results[id].connected = connected;
        ready.fetch_add(1, std::memory_order_release);

        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        if (!connected) return;

        std::mt19937_64 rng(id);
        std::uniform_int_distribution<long> dist(0, num_keys - 1);

        for (long i = 0; i < ops; i++) {
            std::string resp = c.get(make_key(dist(rng)));
            results[id].requests++;
            if (resp.rfind("VALUE", 0) != 0 && resp.rfind("NOTFOUND", 0) != 0)
                results[id].errors++;
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(clients);
    for (int i = 0; i < clients; i++) threads.emplace_back(worker, i);

    while (ready.load(std::memory_order_acquire) < clients) std::this_thread::yield();

    int established = 0;
    for (const Result& r : results) established += r.connected ? 1 : 0;
    if (verbose)
        std::cerr << "Established " << established << " / " << clients
                  << " connections" << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    go.store(true, std::memory_order_release);
    for (std::thread& t : threads) t.join();
    auto end = std::chrono::high_resolution_clock::now();

    std::chrono::duration<double> elapsed = end - start;
    long total_requests = 0;
    long total_errors = 0;
    for (const Result& r : results) {
        total_requests += r.requests;
        total_errors += r.errors;
    }
    double req_per_sec =
        elapsed.count() > 0 ? total_requests / elapsed.count() : 0.0;

    if (verbose)
        std::cout << "clients,total_requests,errors,elapsed_seconds,req_per_sec"
                  << std::endl;
    std::printf("%d,%ld,%ld,%.6f,%.1f\n", clients, total_requests, total_errors,
                elapsed.count(), req_per_sec);

    return total_errors == 0 && established == clients ? 0 : 1;
}
