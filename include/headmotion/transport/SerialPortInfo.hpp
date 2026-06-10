#pragma once

#include <string>

namespace headmotion::transport {

struct SerialPortInfo {
    std::string path;
    std::string display_name;
    std::string symlink_path;
    bool likely_mms = false;
};

} // namespace headmotion::transport