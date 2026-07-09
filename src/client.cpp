#include "client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <sstream>

Client::~Client() { disconnect(); }

bool Client::connect(const std::string& host, uint16_t port) {
    disconnect();

    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        hostent* he = gethostbyname(host.c_str());
        if (!he) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        std::memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

void Client::disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    in_buf_.clear();
}

bool Client::send_all(const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd_, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool Client::read_line(std::string& out) {
    size_t nl = in_buf_.find('\n');
    while (nl == std::string::npos) {
        char buf[4096];
        ssize_t n = recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        in_buf_.append(buf, static_cast<size_t>(n));
        nl = in_buf_.find('\n');
    }
    out = in_buf_.substr(0, nl);
    if (!out.empty() && out.back() == '\r') out.pop_back();
    in_buf_.erase(0, nl + 1);
    return true;
}

std::string Client::command(const std::string& line) {
    if (fd_ < 0) return "";
    if (!send_all(line + "\n")) return "";
    std::string resp;
    if (!read_line(resp)) return "";
    return resp;
}

std::string Client::put(const std::string& key, const std::string& value) {
    return command("PUT " + key + " " + value);
}

std::string Client::get(const std::string& key) {
    return command("GET " + key);
}

std::string Client::contains(const std::string& key) {
    return command("CONTAINS " + key);
}