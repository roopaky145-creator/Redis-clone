#include "PubSubManager.h"

#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

std::size_t PubSubManager::subscribe(SOCKET socket, std::string_view channel)
{
    const std::string channelName(channel);
    std::unique_lock<std::shared_mutex> lock(mutex_);

    std::shared_ptr<std::mutex>& writeLock = socketWriteLocks_[socket];
    if (!writeLock) {
        writeLock = std::make_shared<std::mutex>();
    }

    const bool inserted = channels_[channelName].insert(socket).second;
    std::vector<std::string>& subscriptions = clientSubscriptions_[socket];
    if (inserted) {
        subscriptions.push_back(channelName);
    }

    return subscriptions.size();
}

void PubSubManager::unsubscribeSocket(SOCKET socket)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    const auto subscriptionIterator = clientSubscriptions_.find(socket);
    if (subscriptionIterator != clientSubscriptions_.end()) {
        for (const std::string& channel : subscriptionIterator->second) {
            const auto channelIterator = channels_.find(channel);
            if (channelIterator == channels_.end()) {
                continue;
            }

            channelIterator->second.erase(socket);
            if (channelIterator->second.empty()) {
                channels_.erase(channelIterator);
            }
        }

        clientSubscriptions_.erase(subscriptionIterator);
    }

    socketWriteLocks_.erase(socket);
}

std::size_t PubSubManager::publish(std::string_view channel, std::string_view message)
{
    const std::string frame = messageFrame(channel, message);
    std::vector<SubscriberTarget> targets;
    std::vector<SOCKET> failedSockets;
    std::size_t delivered = 0;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const auto iterator = channels_.find(std::string(channel));
        if (iterator == channels_.end()) {
            return 0;
        }

        targets.reserve(iterator->second.size());
        for (SOCKET socket : iterator->second) {
            const auto writeLockIterator = socketWriteLocks_.find(socket);
            if (writeLockIterator != socketWriteLocks_.end() && writeLockIterator->second) {
                targets.push_back(SubscriberTarget{socket, writeLockIterator->second});
            }
        }
    }

    failedSockets.reserve(targets.size());
    for (const SubscriberTarget& target : targets) {
        std::lock_guard<std::mutex> socketLock(*target.writeMutex);
        if (sendAll(target.socket, frame)) {
            ++delivered;
        } else {
            failedSockets.push_back(target.socket);
        }
    }

    for (SOCKET socket : failedSockets) {
        unsubscribeSocket(socket);
    }

    return delivered;
}

bool PubSubManager::sendToSubscribedSocket(SOCKET socket, std::string_view bytes)
{
    std::shared_ptr<std::mutex> writeLock;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const auto iterator = socketWriteLocks_.find(socket);
        if (iterator == socketWriteLocks_.end() || !iterator->second) {
            return false;
        }

        writeLock = iterator->second;
    }

    std::lock_guard<std::mutex> socketLock(*writeLock);
    return sendAll(socket, bytes);
}

bool PubSubManager::sendAll(SOCKET socket, std::string_view bytes)
{
    std::size_t totalSent = 0;
    while (totalSent < bytes.size()) {
        const std::size_t remaining = bytes.size() - totalSent;
        const int chunkSize = static_cast<int>(
            remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())
                ? static_cast<std::size_t>(std::numeric_limits<int>::max())
                : remaining);

        const int sent = send(socket, bytes.data() + totalSent, chunkSize, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }

        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}

std::string PubSubManager::messageFrame(std::string_view channel, std::string_view message)
{
    std::string frame;
    frame.reserve(channel.size() + message.size() + 64);
    frame.append("*3\r\n");
    frame.append("$7\r\nmessage\r\n");
    frame.push_back('$');
    frame.append(std::to_string(channel.size()));
    frame.append("\r\n");
    frame.append(channel.data(), channel.size());
    frame.append("\r\n");
    frame.push_back('$');
    frame.append(std::to_string(message.size()));
    frame.append("\r\n");
    frame.append(message.data(), message.size());
    frame.append("\r\n");
    return frame;
}
