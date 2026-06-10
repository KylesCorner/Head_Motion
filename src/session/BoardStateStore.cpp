#include "headmotion/session/BoardStateStore.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace headmotion::session {

std::string BoardStateStore::defaultPath() {
    return "data/latest_board_state.bin";
}

void BoardStateStore::save(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes
) {
    if (bytes.empty()) {
        throw std::runtime_error("Refusing to save empty board state");
    }

    const std::filesystem::path file_path{path};

    if (file_path.has_parent_path()) {
        std::filesystem::create_directories(file_path.parent_path());
    }

    std::ofstream out(path, std::ios::binary);

    if (!out) {
        throw std::runtime_error("Failed to open board state file for write: " + path);
    }

    out.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );

    if (!out) {
        throw std::runtime_error("Failed to write board state file: " + path);
    }
}

std::vector<std::uint8_t> BoardStateStore::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);

    if (!in) {
        throw std::runtime_error("Failed to open board state file for read: " + path);
    }

    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    in.seekg(0, std::ios::beg);

    if (size <= 0) {
        throw std::runtime_error("Board state file is empty: " + path);
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));

    in.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );

    if (!in) {
        throw std::runtime_error("Failed to read board state file: " + path);
    }

    return bytes;
}

} // namespace headmotion::session