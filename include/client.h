#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Client {
public:
    Client() = default;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect(const std::string& host, uint16_t port);
    void disconnect();
    bool connected() const { return fd_ >= 0; }

    std::string command(const std::string& line);

    std::string put(const std::string& key, const std::string& value);
    std::string get(const std::string& key);
    std::string contains(const std::string& key);

private:
    int fd_ = -1;
    std::string in_buf_;

    bool send_all(const std::string& data);
    bool read_line(std::string& out);
};
