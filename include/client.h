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

    void set_timeout_ms(int ms);

    std::string command(const std::string& line);

    std::string put(const std::string& key, const std::string& value);
    std::string get(const std::string& key);
    std::string contains(const std::string& key);
    std::string begin(const std::vector<std::string>& keys);
    std::string commit();
    std::string abort();
    std::string shutdown();

private:
    int fd_ = -1;
    std::string in_buf_;
    int timeout_ms_ = 0;

    bool wait_ready(short events);
    bool send_all(const std::string& data);
    bool read_line(std::string& out);
};
