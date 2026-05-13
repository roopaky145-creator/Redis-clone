#pragma once

#include "CommandHandler.h"
#include "PubSubManager.h"

#include <cstdint>
#include <string>

class Server {
public:
    explicit Server(ICommandHandler& commandHandler,
                    std::string bindAddress = "0.0.0.0",
                    std::uint16_t port = 6379);

    // Complexity: O(number_of_clients + total_bytes_processed). Accepted clients
    // are handed to a fixed-size worker pool; each worker uses blocking recv().
    void run();

private:
    ICommandHandler& commandHandler_;
    PubSubManager pubSubManager_;
    std::string bindAddress_;
    std::uint16_t port_;
};
