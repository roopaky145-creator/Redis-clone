#pragma once

#include <winsock2.h>

#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PubSubManager {
public:
    PubSubManager() = default;

    PubSubManager(const PubSubManager&) = delete;
    PubSubManager& operator=(const PubSubManager&) = delete;

    [[nodiscard]] std::size_t subscribe(SOCKET socket, std::string_view channel);
    void unsubscribeSocket(SOCKET socket);
    [[nodiscard]] std::size_t publish(std::string_view channel, std::string_view message);
    [[nodiscard]] bool sendToSubscribedSocket(SOCKET socket, std::string_view bytes);

private:
    struct SubscriberTarget {
        SOCKET socket = INVALID_SOCKET;
        std::shared_ptr<std::mutex> writeMutex;
    };

    [[nodiscard]] static bool sendAll(SOCKET socket, std::string_view bytes);
    [[nodiscard]] static std::string messageFrame(std::string_view channel, std::string_view message);

    std::unordered_map<std::string, std::unordered_set<SOCKET>> channels_;
    std::unordered_map<SOCKET, std::vector<std::string>> clientSubscriptions_;
    std::unordered_map<SOCKET, std::shared_ptr<std::mutex>> socketWriteLocks_;
    mutable std::shared_mutex mutex_;
};
