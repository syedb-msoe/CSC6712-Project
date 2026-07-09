#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "btree.h"

namespace protocol {

static constexpr size_t MAX_TOKEN = KEY_SIZE;

enum class Command {
    PUT,
    GET,
    CONTAINS,
    UNKNOWN,
};

struct Request {
    Command command = Command::UNKNOWN;
    std::vector<std::string> args;
};

inline std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            i++;
        size_t start = i;
        while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            i++;
        if (i > start) tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

inline Request parse(const std::string& line) {
    Request req;
    std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) return req;

    const std::string& verb = tokens[0];
    if (verb == "PUT") req.command = Command::PUT;
    else if (verb == "GET") req.command = Command::GET;
    else if (verb == "CONTAINS") req.command = Command::CONTAINS;
    else req.command = Command::UNKNOWN;

    req.args.assign(tokens.begin() + 1, tokens.end());
    return req;
}

inline void to_fixed(const std::string& s, char* out, size_t width) {
    std::memset(out, 0, width);
    std::memcpy(out, s.data(), std::min(s.size(), width));
}

inline std::string from_fixed(const std::string& v) {
    size_t pos = v.find('\0');
    return pos == std::string::npos ? v : v.substr(0, pos);
}

}
