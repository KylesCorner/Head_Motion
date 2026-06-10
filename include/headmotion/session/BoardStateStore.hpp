#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::session {

class BoardStateStore {
public:
    static std::string defaultPath();

    static void save(
        const std::string& path,
        const std::vector<std::uint8_t>& bytes
    );

    static std::vector<std::uint8_t> load(const std::string& path);
};

} // namespace headmotion::session