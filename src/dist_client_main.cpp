#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dist_client.h"

void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog
        << " <R> <host:port> [host:port ...] [--qr N] [--qw N] [--timeout MS]\n\n"
        << "  <R>          replica set size (each key is stored on R servers)\n"
        << "  <host:port>  one entry per server (need at least R of them)\n"
        << "  --qr N       read quorum   (default: R - Qw + 1)\n"
        << "  --qw N       write quorum  (default: floor(R/2) + 1)\n"
        << "  --timeout MS per-request socket timeout in ms (default 2000)\n\n"
        << "Reads commands from stdin, one per line:\n"
        << "  PUT <key> <value>\n"
        << "  GET <key>\n"
        << "  INFO            show R / Qr / Qw and reachable servers\n"
        << "  QUIT\n";
}

std::string get_status(DistributedClient::GetResult::Status s) {
    switch (s) {
        case DistributedClient::GetResult::OK: return "OK";
        case DistributedClient::GetResult::NOT_FOUND: return "NOT_FOUND";
        default: return "INCONSISTENT";
    }
}

int main(int argc, char* argv[]) {
    std::vector<std::string> servers;
    long R = -1;
    int qr = 0, qw = 0, timeout_ms = 2000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--qr") {
            qr = std::atoi(next("--qr"));
        } else if (a == "--qw") {
            qw = std::atoi(next("--qw"));
        } else if (a == "--timeout") {
            timeout_ms = std::atoi(next("--timeout"));
        } else if (R < 0) {
            R = std::strtol(a.c_str(), nullptr, 10);
            if (R <= 0) {
                std::cerr << "R must be a positive integer\n";
                return 1;
            }
        } else {
            servers.push_back(a);
        }
    }

    if (R < 0 || servers.empty()) {
        usage(argv[0]);
        return 1;
    }

    std::unique_ptr<DistributedClient> db;
    try {
        db = std::make_unique<DistributedClient>(
            static_cast<size_t>(R), servers, qr, qw, timeout_ms);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cerr << "Distributed client ready: R=" << db->replica_size()
              << " Qr=" << db->read_quorum() << " Qw=" << db->write_quorum()
              << " (" << db->reachable() << "/" << servers.size()
              << " servers reachable). Type commands (Ctrl-D to quit).\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd;
        if (!(is >> cmd)) continue;

        if (cmd == "QUIT" || cmd == "EXIT") {
            break;
        } else if (cmd == "INFO") {
            std::cout << "R=" << db->replica_size()
                      << " Qr=" << db->read_quorum()
                      << " Qw=" << db->write_quorum()
                      << " reachable=" << db->reachable() << "/"
                      << servers.size() << std::endl;
        } else if (cmd == "GET") {
            std::string key;
            if (!(is >> key)) {
                std::cout << "ERR usage: GET <key>" << std::endl;
                continue;
            }
            DistributedClient::GetResult r = db->get(key);
            std::cout << get_status(r.status);
            if (r.status == DistributedClient::GetResult::OK) {
                std::cout << " " << r.value;
            }
            std::cout << " (agree=" << r.agree << "/" << db->read_quorum()
                      << ", responses=" << r.responses << ")" << std::endl;
        } else if (cmd == "PUT") {
            std::string key, value;
            if (!(is >> key >> value)) {
                std::cout << "ERR usage: PUT <key> <value>" << std::endl;
                continue;
            }
            DistributedClient::PutResult r = db->put(key, value);
            switch (r.status) {
                case DistributedClient::PutResult::COMMITTED:
                    std::cout << "COMMITTED";
                    break;
                case DistributedClient::PutResult::ABORTED_CONSISTENT:
                    std::cout << "FAILED (store consistent, no change)";
                    break;
                case DistributedClient::PutResult::INCONSISTENT:
                    std::cout << "FAILED (store INCONSISTENT)";
                    break;
            }
            std::cout << " (prepared=" << r.prepared
                      << ", committed=" << r.committed << ", Qw="
                      << db->write_quorum() << ")" << std::endl;
        } else {
            std::cout << "ERR unknown command: " << cmd << std::endl;
        }
    }

    return 0;
}