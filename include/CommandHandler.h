#pragma once

#include <string>

struct RespValue;

class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;

    // Complexity: command execution is implementation-defined. The server treats
    // this as an opaque O(reply_size) producer and never inspects storage state.
    [[nodiscard]] virtual std::string handleCommand(const RespValue& request) = 0;
};
