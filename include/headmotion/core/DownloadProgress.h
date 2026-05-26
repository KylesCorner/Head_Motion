#pragma once

#include <cstdint>

namespace headmotion {

struct DownloadProgress {
    uint32_t entriesLeft = 0;
    uint32_t totalEntries = 0;
};

}  // namespace headmotion
