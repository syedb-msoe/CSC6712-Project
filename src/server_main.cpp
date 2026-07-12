#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "btree.h"
#include "btree_server.h"

BTreeServer* server_pointer = nullptr;

void on_signal(int) {
    if (server_pointer) server_pointer->request_shutdown();
}

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  -p, --port <port>        TCP port to listen on (default 5555)\n"
              << "  -d, --db <path>          B-tree database file (default default.db)\n"
              << "  -v, --verbose            enable verbose logging\n"
              << "  -h, --help               show this help\n";
}

int main(int argc, char* argv[]) {
    uint16_t port = 5555;
    std::string db_path = "default.db";
    bool verbose = false;

    static struct option long_opts[] = {
        {"port", required_argument, nullptr, 'p'},
        {"db", required_argument, nullptr, 'd'},
        {"verbose", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:d:w:vh", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'p': port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'd': db_path = optarg; break;
            case 'v': verbose = true; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    struct stat st{};
    if (stat(db_path.c_str(), &st) != 0) {
        if (!BTreeDB::create(db_path)) {
            std::cerr << "Failed to create database file: " << db_path << std::endl;
            return 1;
        }
    }

    try {
        BTreeServer server(db_path);
        server.set_verbose(verbose);

        if (!server.listen_on(port)) {
            std::cerr << "Failed to listen on port " << port << std::endl;
            return 1;
        }

        server_pointer = &server;
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        signal(SIGPIPE, SIG_IGN);

        std::cerr << "B-tree server listening on port " << server.bound_port()
                  << " (db=" << db_path << ")" << std::endl;

        server.run();
        std::cerr << "Server shut down cleanly." << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
