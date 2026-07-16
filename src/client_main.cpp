#include <getopt.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "client.h"

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  -H, --host <host>   server host (default 127.0.0.1)\n"
              << "  -p, --port <port>   server port (default 5555)\n"
              << "  -d, --debug         echo raw requests/responses\n"
              << "  -h, --help          show this help\n\n"
              << "Reads protocol commands from stdin, one per line, and prints\n"
              << "the server's responses. Example commands:\n"
              << "  PUT alpha 1\n"
              << "  GET alpha\n"
              << "  BEGIN alpha beta\n"
              << "  COMMIT\n"
              << "  SHUTDOWN\n";
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 5555;
    bool debug = false;

    static struct option long_opts[] = {
        {"host", required_argument, nullptr, 'H'},
        {"port", required_argument, nullptr, 'p'},
        {"debug", no_argument, nullptr, 'd'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "H:p:dh", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'H': host = optarg; break;
            case 'p': port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'd': debug = true; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    Client client;
    if (!client.connect(host, port)) {
        std::cerr << "Failed to connect to " << host << ":" << port << std::endl;
        return 1;
    }
    std::cerr << "Connected to " << host << ":" << port
              << ". Type commands (Ctrl-D to quit).\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (debug) std::cerr << ">> " << line << std::endl;

        std::string resp = client.command(line);
        if (resp.empty() && !client.connected()) {
            std::cerr << "Connection closed by server." << std::endl;
            break;
        }
        std::cout << resp << std::endl;
        if (debug) std::cerr << "<< " << resp << std::endl;
    }

    return 0;
}
