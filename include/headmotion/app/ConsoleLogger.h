#pragma once

#include "headmotion/interfaces/ILogger.h"

#include <iostream>
#include <string>

namespace headmotion {

class ConsoleLogger : public ILogger {
public:
    void info(const std::string& message) override {
        std::cout << "[INFO] " << message << "\n";
    }

    void warn(const std::string& message) override {
        std::cerr << "[WARN] " << message << "\n";
    }

    void error(const std::string& message) override {
        std::cerr << "[ERROR] " << message << "\n";
    }
};

}  // namespace headmotion
