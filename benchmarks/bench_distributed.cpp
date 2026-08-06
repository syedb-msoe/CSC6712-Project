#include <getopt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dist_client.h"

static std::vector<int> parse_int_list(const std::string& arg) {
    std::vector<int> values;
    std::stringstream ss(arg);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        values.push_back(std::atoi(item.c_str()));
    }
    return values;
}

static std::string make_key(const std::string& prefix, uint64_t id) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s_%020lu", prefix.c_str(), id);
    return std::string(buf);
}

static std::string make_value(uint64_t id) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "val_%020lu", id);
    return std::string(buf);
}

static bool valid_quorum(int R, int qr, int qw) {
    return qr > 0 && qw > 0 && qr <= R && qw <= R && qr + qw > R && 2 * qw > R;
}

static double throughput(long requests, double seconds) {
    return seconds > 0.0 ? static_cast<double>(requests) / seconds : 0.0;
}

struct BenchResult {
    double elapsed_seconds = 0.0;
    long errors = 0;
    long requests = 0;
};

static BenchResult benchmark_writes(DistributedClient& db, int count,
                                    const std::string& prefix) {
    BenchResult result;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < count; ++i) {
        auto key = make_key(prefix, i);
        auto value = make_value(i);
        auto r = db.put(key, value);
        ++result.requests;
        if (r.status != DistributedClient::PutResult::COMMITTED) {
            ++result.errors;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds =
        std::chrono::duration<double>(end - start).count();
    return result;
}

static BenchResult benchmark_reads(DistributedClient& db, int count,
                                   const std::string& prefix) {
    BenchResult result;
    std::vector<int> indices(count);
    for (int i = 0; i < count; ++i) indices[i] = i;
    std::mt19937 rng(42);
    std::shuffle(indices.begin(), indices.end(), rng);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < count; ++i) {
        auto key = make_key(prefix, indices[i]);
        auto r = db.get(key);
        ++result.requests;
        if (r.status != DistributedClient::GetResult::OK) {
            ++result.errors;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    result.elapsed_seconds =
        std::chrono::duration<double>(end - start).count();
    return result;
}

static BenchResult benchmark_read_scaling(const std::vector<std::string>&
                                              endpoints,
                                          int R, int qr, int qw,
                                          int timeout_ms,
                                          int clients,
                                          int ops_per_client,
                                          int num_keys,
                                          uint32_t seed) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<long> total_requests{0};
    std::atomic<long> total_errors{0};

    struct ThreadResult {
        bool connected = false;
        std::string error;
    };

    std::vector<ThreadResult> thread_results(clients);
    std::vector<std::thread> threads;
    threads.reserve(clients);

    for (int id = 0; id < clients; ++id) {
        threads.emplace_back([&, id]() {
            try {
                DistributedClient db(R, endpoints, qr, qw, timeout_ms, seed + id);
                thread_results[id].connected = db.reachable() >= R;
                if (!thread_results[id].connected) {
                    thread_results[id].error = "insufficient reachable replicas";
                }
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                if (!thread_results[id].connected) return;

                std::mt19937_64 rng(seed + id);
                std::uniform_int_distribution<int> dist(0, num_keys - 1);
                for (int op = 0; op < ops_per_client; ++op) {
                    auto key = make_key("scale", dist(rng));
                    auto r = db.get(key);
                    total_requests.fetch_add(1, std::memory_order_relaxed);
                    if (r.status != DistributedClient::GetResult::OK) {
                        total_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (const std::exception& e) {
                thread_results[id].connected = false;
                thread_results[id].error = e.what();
                ready.fetch_add(1, std::memory_order_release);
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < clients)
        std::this_thread::yield();

    BenchResult result;
    auto start = std::chrono::high_resolution_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    auto end = std::chrono::high_resolution_clock::now();

    result.elapsed_seconds =
        std::chrono::duration<double>(end - start).count();
    result.requests = total_requests.load();
    result.errors = total_errors.load();
    return result;
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <host:port> [host:port ...] [options]\n"
              << "  --r-values LIST   replica sizes to benchmark (default: 3,5,7)\n"
              << "  --scale-r N       replica size used for read scaling tests (default first R value)\n"
              << "  --qr-values LIST  read quorum values for scaling tests (default: all valid)\n"
              << "  --qw-values LIST  write quorum values for quorum tests (default: all valid)\n"
              << "  --clients LIST    client counts for read scaling (default: 1,2,4,8)\n"
              << "  --write-ops N     per-trial write count (default: 1000)\n"
              << "  --read-ops N      per-trial read count (default: 1000)\n"
              << "  --scale-keys N    number of preloaded keys for read scaling (default: 10000)\n"
              << "  --scale-ops N     per-client read ops for scaling (default: 1000)\n"
              << "  --trials N        trials per configuration (default: 3)\n"
              << "  --timeout MS      per-request socket timeout in ms (default: 2000)\n"
              << "  --mode MODE       all|quorum|scaling (default: all)\n"
              << "  --verbose         enable verbose logging\n";
}

int main(int argc, char* argv[]) {
    std::vector<std::string> endpoints;
    std::vector<int> r_values = {3, 5, 7};
    std::vector<int> qr_values;
    std::vector<int> qw_values;
    std::vector<int> client_counts = {1, 2, 4, 8};
    int scale_R = -1;
    int write_ops = 1000;
    int read_ops = 1000;
    int scale_keys = 10000;
    int scale_ops = 1000;
    int trials = 3;
    int timeout_ms = 2000;
    bool verbose = false;
    std::string mode = "all";

    static struct option long_opts[] = {
        {"r-values", required_argument, nullptr, 'r'},
        {"scale-r", required_argument, nullptr, 'R'},
        {"qr-values", required_argument, nullptr, 'q'},
        {"qw-values", required_argument, nullptr, 'w'},
        {"clients", required_argument, nullptr, 'c'},
        {"write-ops", required_argument, nullptr, 'W'},
        {"read-ops", required_argument, nullptr, 'L'},
        {"scale-keys", required_argument, nullptr, 'k'},
        {"scale-ops", required_argument, nullptr, 's'},
        {"trials", required_argument, nullptr, 't'},
        {"timeout", required_argument, nullptr, 'T'},
        {"mode", required_argument, nullptr, 'm'},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hr:R:q:w:c:W:L:k:s:t:T:m:vh", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'h':usage(argv[0]);
                return 0;
            case 'r':
                r_values = parse_int_list(optarg);
                break;
            case 'R':
                scale_R = std::atoi(optarg);
                break;
            case 'q':
                qr_values = parse_int_list(optarg);
                break;
            case 'w':
                qw_values = parse_int_list(optarg);
                break;
            case 'c':
                client_counts = parse_int_list(optarg);
                break;
            case 'W':
                write_ops = std::atoi(optarg);
                break;
            case 'L':
                read_ops = std::atoi(optarg);
                break;
            case 'k':
                scale_keys = std::atoi(optarg);
                break;
            case 's':
                scale_ops = std::atoi(optarg);
                break;
            case 't':
                trials = std::atoi(optarg);
                break;
            case 'T':
                timeout_ms = std::atoi(optarg);
                break;
            case 'm':
                mode = optarg;
                break;
            case 'v':
                verbose = true;
                break;
            default:
                break;
        }
    }

    for (int i = optind; i < argc; ++i) {
        endpoints.emplace_back(argv[i]);
    }

    if (endpoints.empty()) {
        std::cerr << "At least one endpoint is required.\n";
        usage(argv[0]);
        return 1;
    }

    if (r_values.empty()) {
        std::cerr << "At least one replica size is required.\n";
        return 1;
    }
    if (scale_R < 0) scale_R = r_values.front();

    std::cout << "scenario,R,qr,qw,clients,ops,trial,elapsed_seconds,ops_per_sec,errors" << std::endl;

    if (mode == "all" || mode == "quorum") {
        for (int R : r_values) {
            if (static_cast<int>(endpoints.size()) < R) {
                if (verbose) {
                    std::cerr << "Skipping R=" << R << " because only "
                              << endpoints.size() << " endpoints were provided.\n";
                }
                continue;
            }

            std::vector<int> use_qr = qr_values;
            std::vector<int> use_qw = qw_values;
            if (use_qr.empty() || use_qw.empty()) {
                use_qr.clear();
                use_qw.clear();
                for (int qr = 1; qr <= R; ++qr) use_qr.push_back(qr);
                for (int qw = 1; qw <= R; ++qw) use_qw.push_back(qw);
            }

            for (int qr : use_qr) {
                for (int qw : use_qw) {
                    if (!valid_quorum(R, qr, qw)) continue;
                    for (int trial = 1; trial <= trials; ++trial) {
                        try {
                            DistributedClient db(static_cast<size_t>(R), endpoints,
                                                  qr, qw, timeout_ms,
                                                  static_cast<uint32_t>(trial));
                            if (db.reachable() < R) {
                                if (verbose) {
                                    std::cerr << "Skipping R=" << R << " qr=" << qr
                                              << " qw=" << qw
                                              << " because not all replicas are reachable.\n";
                                }
                                break;
                            }

                            auto write_result = benchmark_writes(db, write_ops,
                                                                "quorum");
                            std::cout << "quorum_write," << R << "," << qr << ","
                                      << qw << ",1," << write_ops << "," << trial
                                      << "," << write_result.elapsed_seconds << ","
                                      << throughput(write_result.requests, write_result.elapsed_seconds)
                                      << "," << write_result.errors << std::endl;

                            auto read_result = benchmark_reads(db, read_ops,
                                                              "quorum");
                            std::cout << "quorum_read," << R << "," << qr << ","
                                      << qw << ",1," << read_ops << "," << trial
                                      << "," << read_result.elapsed_seconds << ","
                                      << throughput(read_result.requests, read_result.elapsed_seconds)
                                      << "," << read_result.errors << std::endl;
                        } catch (const std::exception& e) {
                            if (verbose) {
                                std::cerr << "Skipping R=" << R << " qr=" << qr
                                          << " qw=" << qw << ": " << e.what()
                                          << std::endl;
                            }
                        }
                    }
                }
            }
        }
    }

    if (mode == "all" || mode == "scaling") {
        int R = scale_R;
        if (static_cast<int>(endpoints.size()) < R) {
            std::cerr << "Cannot run scaling benchmark: need at least " << R
                      << " endpoints.\n";
        } else {
            int qw = R / 2 + 1;
            std::vector<int> use_qr = qr_values;
            if (use_qr.empty()) {
                use_qr.clear();
                for (int qr = 1; qr <= R; ++qr) {
                    if (valid_quorum(R, qr, qw)) use_qr.push_back(qr);
                }
            }

            for (int qr : use_qr) {
                if (!valid_quorum(R, qr, qw)) continue;

                if (verbose) {
                    std::cerr << "Preparing " << scale_keys
                              << " keys for read-scaling benchmark with R=" << R
                              << " qr=" << qr << " qw=" << qw << "...\n";
                }
                try {
                    DistributedClient preloader(static_cast<size_t>(R), endpoints,
                                                 qr, qw, timeout_ms, 0);
                    if (preloader.reachable() < R) {
                        std::cerr << "Skipping scaling benchmark for qr=" << qr
                                  << ": not all replicas are reachable.\n";
                        continue;
                    }
                    auto preload = benchmark_writes(preloader, scale_keys,
                                                   "scale");
                    if (preload.errors > 0) {
                        std::cerr << "Preload errors for qr=" << qr
                                  << " qw=" << qw << ", errors=" << preload.errors
                                  << ".\n";
                        continue;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Scaling preload failed for qr=" << qr
                              << ": " << e.what() << std::endl;
                    continue;
                }

                for (int clients : client_counts) {
                    for (int trial = 1; trial <= trials; ++trial) {
                        auto result = benchmark_read_scaling(
                            endpoints, R, qr, qw, timeout_ms, clients,
                            scale_ops, scale_keys, static_cast<uint32_t>(trial));
                        std::cout << "read_scaling," << R << "," << qr << ","
                                  << qw << "," << clients << ","
                                  << (clients * scale_ops) << "," << trial << ","
                                  << result.elapsed_seconds << ","
                                  << throughput(result.requests, result.elapsed_seconds) << ","
                                  << result.errors << std::endl;
                    }
                }
            }
        }
    }

    return 0;
}
