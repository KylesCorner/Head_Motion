#pragma once

#include "headmotion/core/HeadMotionSample.h"
#include "headmotion/core/Result.h"
#include "headmotion/core/SessionMetadata.h"

#include <cstddef>

namespace headmotion {

class ISampleSink {
public:
    virtual ~ISampleSink() = default;

    virtual Result open(const SessionMetadata& metadata) = 0;
    virtual Result write(const ImuSample& sample) = 0;
    virtual Result closeAndCommit() = 0;

    virtual size_t sampleCount() const = 0;
};

}  // namespace headmotion
