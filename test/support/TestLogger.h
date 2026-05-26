#pragma once

#include "headmotion/interfaces/ILogger.h"

#include <string>
#include <vector>

namespace headmotion::test {

class TestLogger : public ILogger {
public:
    std::vector<std::string> infoMessages;
    std::vector<std::string> warnMessages;
    std::vector<std::string> errorMessages;

    void info(const std::string& message) override {
        infoMessages.push_back(message);
    }

    void warn(const std::string& message) override {
        warnMessages.push_back(message);
    }

    void error(const std::string& message) override {
        errorMessages.push_back(message);
    }
};

}  // namespace headmotion::test
