#pragma once

#include "headmotion/interfaces/ISampleSink.h"

#include <vector>

namespace headmotion::test {

class FakeSampleSink : public ISampleSink {
public:
    bool openCalled = false;
    bool closeAndCommitCalled = false;

    Result openResult = Result::success();
    Result writeResult = Result::success();
    Result closeAndCommitResult = Result::success();

    SessionMetadata openedMetadata;
    std::vector<ImuSample> samples;

    Result open(const SessionMetadata& metadata) override {
        openCalled = true;
        openedMetadata = metadata;
        return openResult;
    }

    Result write(const ImuSample& sample) override {
        if (!writeResult.ok()) {
            return writeResult;
        }

        samples.push_back(sample);
        return Result::success();
    }

    Result closeAndCommit() override {
        closeAndCommitCalled = true;
        return closeAndCommitResult;
    }

    size_t sampleCount() const override {
        return samples.size();
    }
};

}  // namespace headmotion::test
