#include "headmotion/util/Hex.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace headmotion::util {

namespace {

int hexValue(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);

    if (uc >= '0' && uc <= '9') {
        return uc - '0';
    }

    if (uc >= 'a' && uc <= 'f') {
        return 10 + (uc - 'a');
    }

    if (uc >= 'A' && uc <= 'F') {
        return 10 + (uc - 'A');
    }

    return -1;
}

bool isSeparator(char c) {
    return std::isspace(static_cast<unsigned char>(c)) ||
           c == ',' ||
           c == ':' ||
           c == '-';
}

} // namespace

std::vector<std::uint8_t> parseHexBytes(const std::string& input) {
    std::string compact;

    compact.reserve(input.size());

    for (char c : input) {
        if (isSeparator(c)) {
            continue;
        }

        if (c == 'x' || c == 'X') {
            if (!compact.empty() && compact.back() == '0') {
                compact.pop_back();
                continue;
            }
        }

        if (hexValue(c) < 0) {
            throw std::runtime_error("Invalid hex character: " + std::string(1, c));
        }

        compact.push_back(c);
    }

    if (compact.empty()) {
        return {};
    }

    if ((compact.size() % 2) != 0) {
        throw std::runtime_error("Hex string must contain an even number of hex digits");
    }

    std::vector<std::uint8_t> out;
    out.reserve(compact.size() / 2);

    for (std::size_t i = 0; i < compact.size(); i += 2) {
        const int hi = hexValue(compact[i]);
        const int lo = hexValue(compact[i + 1]);

        if (hi < 0 || lo < 0) {
            throw std::runtime_error("Invalid hex byte");
        }

        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }

    return out;
}

std::string hexDump(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream oss;

    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }

        oss << std::uppercase
            << std::hex
            << std::setw(2)
            << std::setfill('0')
            << static_cast<int>(bytes[i]);
    }

    return oss.str();
}

std::string asciiPreview(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve(bytes.size());

    for (const auto b : bytes) {
        const unsigned char c = static_cast<unsigned char>(b);

        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (std::isprint(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('.');
        }
    }

    return out;
}

} // namespace headmotion::util