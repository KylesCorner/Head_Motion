#pragma once

#include <string>
#include <utility>

namespace headmotion {

class Result {
public:
    static Result success() {
        return Result(true, "");
    }

    static Result error(std::string message) {
        return Result(false, std::move(message));
    }

    bool ok() const {
        return ok_;
    }

    explicit operator bool() const {
        return ok_;
    }

    const std::string& message() const {
        return message_;
    }

private:
    Result(bool ok, std::string message)
        : ok_(ok),
          message_(std::move(message)) {}

    bool ok_ = false;
    std::string message_;
};

}  // namespace headmotion
