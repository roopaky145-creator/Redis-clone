#pragma once

#include "CommandHandler.h"
#include "RespParser.h"
#include "WalLogger.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

class Store final : public ICommandHandler {
public:
    explicit Store(std::size_t maxKeys = 100'000, std::string walPath = "redis_clone.wal");
    ~Store() override;

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    // Complexity: dispatch is O(argument_count + command_name_length). Cache
    // operations are O(1) average for hash lookup plus O(1) list splicing.
    [[nodiscard]] std::string handleCommand(const RespValue& request) override;

private:
    using Clock = std::chrono::steady_clock;
    using WallClock = std::chrono::system_clock;
    using ListValue = std::deque<std::string>;
    using Value = std::variant<std::string, ListValue>;

    struct CacheNode {
        Value value;
        std::list<std::string>::iterator lruIterator;
        std::optional<Clock::time_point> expiresAt;
    };

    using CacheMap = std::unordered_map<std::string, CacheNode>;

    [[nodiscard]] std::string handlePing(const std::vector<RespValue>& arguments) const;
    [[nodiscard]] std::string handleSet(const std::vector<RespValue>& arguments);
    [[nodiscard]] std::string handleGet(const std::vector<RespValue>& arguments);
    [[nodiscard]] std::string handleDel(const std::vector<RespValue>& arguments);
    [[nodiscard]] std::string handleExpire(const std::vector<RespValue>& arguments);
    [[nodiscard]] std::string handleTtl(const std::vector<RespValue>& arguments);
    [[nodiscard]] std::string handleLpush(const std::vector<RespValue>& arguments);
    [[nodiscard]] std::string handleRpop(const std::vector<RespValue>& arguments);
    [[nodiscard]] std::string handleLrange(const std::vector<RespValue>& arguments);

    void recover();
    void applyRecoveredCommand(const RespValue& value);
    void setEntryLocked(std::string_view key, std::string_view value);
    void lpushEntryLocked(std::string_view key, const std::vector<std::string_view>& values);
    void rpopEntryLocked(std::string_view key);
    void delEntriesLocked(const std::vector<std::string>& keys);
    void expireAtMillisLocked(std::string_view key, std::int64_t expiresAtMillis);

    void touch(CacheMap::iterator iterator);
    void eraseEntry(CacheMap::iterator iterator);
    void activeExpiryLoop();
    void expireSample();

    [[nodiscard]] bool isExpired(const CacheNode& node, Clock::time_point now) const;
    [[nodiscard]] bool parseInt64(std::string_view text, std::int64_t& output) const;
    [[nodiscard]] bool computeExpiryTime(Clock::time_point now,
                                         std::int64_t ttlSeconds,
                                         Clock::time_point& output) const;
    [[nodiscard]] bool computeExpiryMillis(std::int64_t ttlSeconds,
                                           std::int64_t& expiresAtMillis) const;

    [[nodiscard]] static bool argumentText(const RespValue& value, std::string_view& output);
    [[nodiscard]] static bool equalsAsciiInsensitive(std::string_view left, std::string_view right);
    [[nodiscard]] static bool holdsString(const CacheNode& node);
    [[nodiscard]] static bool holdsList(const CacheNode& node);
    [[nodiscard]] static std::string simpleString(std::string_view value);
    [[nodiscard]] static std::string error(std::string_view value);
    [[nodiscard]] static std::string wrongTypeError();
    [[nodiscard]] static std::string integer(std::int64_t value);
    [[nodiscard]] static std::string bulkString(std::string_view value);
    [[nodiscard]] static std::string nullBulkString();
    [[nodiscard]] static std::string arrayBulkStrings(const ListValue& values,
                                                      std::size_t start,
                                                      std::size_t stop);
    [[nodiscard]] static std::string emptyArray();

    static constexpr std::size_t kExpiryScanBudget = 256;
    static constexpr std::chrono::milliseconds kExpiryInterval{100};

    std::size_t maxKeys_;
    WalLogger walLogger_;
    std::list<std::string> lruList_;
    CacheMap data_;

    mutable std::shared_mutex rw_mutex_;
    std::mutex expiryWakeMutex_;
    std::condition_variable expiryWakeCondition_;
    bool stopExpiryThread_ = false;
    std::thread expiryThread_;
};
