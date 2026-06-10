#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace headmotion::util {

std::vector<std::uint8_t> parseHexBytes(const std::string& input);
std::string hexDump(const std::vector<std::uint8_t>& bytes);
std::string asciiPreview(const std::vector<std::uint8_t>& bytes);

} // namespace headmotion::util