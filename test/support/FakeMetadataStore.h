#pragma once

#include "headmotion/interfaces/IMetadataStore.h"

#include <optional>
#include <string>

namespace headmotion::test {

class FakeMetadataStore : public IMetadataStore {
public:
    bool saveCalled = false;
    bool markDownloadedCalled = false;

    Result saveResult = Result::success();
    Result markDownloadedResult = Result::success();

    std::optional<SessionMetadata> savedMetadata;
    std::string markedDownloadedSessionId;

    Result save(const SessionMetadata& metadata) override {
        saveCalled = true;
        savedMetadata = metadata;
        return saveResult;
    }

    std::optional<SessionMetadata>
    loadLatestForDevice(const std::string&) override {
        return savedMetadata;
    }

    Result markDownloaded(const std::string& sessionId) override {
        markDownloadedCalled = true;
        markedDownloadedSessionId = sessionId;
        return markDownloadedResult;
    }
};

}  // namespace headmotion::test
