#include "Store.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::size_t decimalDigits(std::size_t value) noexcept
{
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }

    return digits;
}

} // namespace

Store::Store(std::size_t maxKeys, std::string walPath)
    : maxKeys_(maxKeys),
      walLogger_(std::move(walPath))
{
    if (maxKeys_ == 0) {
        throw std::invalid_argument("Store maxKeys must be greater than zero");
    }

    data_.reserve(maxKeys_);
    recover();
    walLogger_.start();

    expiryThread_ = std::thread([this]() {
        activeExpiryLoop();
    });
}

Store::~Store()
{
    {
        std::lock_guard<std::mutex> lock(expiryWakeMutex_);
        stopExpiryThread_ = true;
    }

    expiryWakeCondition_.notify_all();

    if (expiryThread_.joinable()) {
        expiryThread_.join();
    }
}

std::string Store::handleCommand(const RespValue& request)
{
    if (request.type != RespType::Array || request.isNull) {
        return error("command must be a non-null RESP array");
    }

    const std::vector<RespValue>& arguments = request.elements;
    if (arguments.empty()) {
        return error("empty command array");
    }

    std::string_view command;
    if (!argumentText(arguments.front(), command)) {
        return error("command name must be a non-null bulk string");
    }

    if (equalsAsciiInsensitive(command, "PING")) {
        return handlePing(arguments);
    }

    if (equalsAsciiInsensitive(command, "SET")) {
        return handleSet(arguments);
    }

    if (equalsAsciiInsensitive(command, "GET")) {
        return handleGet(arguments);
    }

    if (equalsAsciiInsensitive(command, "DEL")) {
        return handleDel(arguments);
    }

    if (equalsAsciiInsensitive(command, "EXPIRE")) {
        return handleExpire(arguments);
    }

    if (equalsAsciiInsensitive(command, "TTL")) {
        return handleTtl(arguments);
    }

    if (equalsAsciiInsensitive(command, "LPUSH")) {
        return handleLpush(arguments);
    }

    if (equalsAsciiInsensitive(command, "RPOP")) {
        return handleRpop(arguments);
    }

    if (equalsAsciiInsensitive(command, "LRANGE")) {
        return handleLrange(arguments);
    }

    return error("unknown command");
}

std::string Store::handlePing(const std::vector<RespValue>& arguments) const
{
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    if (arguments.size() == 1) {
        return simpleString("PONG");
    }

    if (arguments.size() == 2) {
        std::string_view message;
        if (!argumentText(arguments[1], message)) {
            return error("PING argument must be a non-null bulk string");
        }

        return bulkString(message);
    }

    return error("wrong number of arguments for 'ping' command");
}

std::string Store::handleSet(const std::vector<RespValue>& arguments)
{
    if (arguments.size() != 3) {
        return error("wrong number of arguments for 'set' command");
    }

    std::string_view key;
    std::string_view value;
    if (!argumentText(arguments[1], key) || !argumentText(arguments[2], value)) {
        return error("SET key and value must be non-null bulk strings");
    }

    const std::string keyString(key);
    const std::string valueString(value);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    const std::vector<std::string_view> walCommand{
        "SET",
        std::string_view(keyString.data(), keyString.size()),
        std::string_view(valueString.data(), valueString.size())
    };

    if (!walLogger_.append(walCommand)) {
        return error("failed to write WAL");
    }

    setEntryLocked(keyString, valueString);
    return simpleString("OK");
}

std::string Store::handleGet(const std::vector<RespValue>& arguments)
{
    if (arguments.size() != 2) {
        return error("wrong number of arguments for 'get' command");
    }

    std::string_view key;
    if (!argumentText(arguments[1], key)) {
        return error("GET key must be a non-null bulk string");
    }

    const std::string keyString(key);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator == data_.end()) {
        return nullBulkString();
    }

    const Clock::time_point now = Clock::now();
    if (isExpired(iterator->second, now)) {
        eraseEntry(iterator);
        return nullBulkString();
    }

    if (!holdsString(iterator->second)) {
        return wrongTypeError();
    }

    touch(iterator);
    const std::string& value = std::get<std::string>(iterator->second.value);
    return bulkString(std::string_view(value.data(), value.size()));
}

std::string Store::handleDel(const std::vector<RespValue>& arguments)
{
    if (arguments.size() < 2) {
        return error("wrong number of arguments for 'del' command");
    }

    std::vector<std::string> keys;
    keys.reserve(arguments.size() - 1);

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        std::string_view key;
        if (!argumentText(arguments[index], key)) {
            return error("DEL keys must be non-null bulk strings");
        }

        keys.emplace_back(key);
    }

    std::vector<std::string_view> walCommand;
    walCommand.reserve(keys.size() + 1);
    walCommand.emplace_back("DEL");
    for (const std::string& key : keys) {
        walCommand.emplace_back(key.data(), key.size());
    }

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    if (!walLogger_.append(walCommand)) {
        return error("failed to write WAL");
    }

    std::int64_t deletedCount = 0;
    const Clock::time_point now = Clock::now();

    for (const std::string& key : keys) {
        CacheMap::iterator iterator = data_.find(key);
        if (iterator == data_.end()) {
            continue;
        }

        if (isExpired(iterator->second, now)) {
            eraseEntry(iterator);
            continue;
        }

        eraseEntry(iterator);
        ++deletedCount;
    }

    return integer(deletedCount);
}

std::string Store::handleExpire(const std::vector<RespValue>& arguments)
{
    if (arguments.size() != 3) {
        return error("wrong number of arguments for 'expire' command");
    }

    std::string_view key;
    std::string_view secondsText;
    if (!argumentText(arguments[1], key) || !argumentText(arguments[2], secondsText)) {
        return error("EXPIRE key and seconds must be non-null bulk strings");
    }

    std::int64_t ttlSeconds = 0;
    if (!parseInt64(secondsText, ttlSeconds)) {
        return error("value is not an integer or out of range");
    }

    const std::string keyString(key);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator == data_.end()) {
        return integer(0);
    }

    const Clock::time_point now = Clock::now();
    if (isExpired(iterator->second, now)) {
        eraseEntry(iterator);
        return integer(0);
    }

    if (ttlSeconds <= 0) {
        const std::vector<std::string_view> walCommand{
            "EXPIREAT_MS",
            std::string_view(keyString.data(), keyString.size()),
            "0"
        };

        if (!walLogger_.append(walCommand)) {
            return error("failed to write WAL");
        }

        eraseEntry(iterator);
        return integer(1);
    }

    Clock::time_point expiresAt;
    if (!computeExpiryTime(now, ttlSeconds, expiresAt)) {
        return error("expire time is out of range");
    }

    std::int64_t expiresAtMillis = 0;
    if (!computeExpiryMillis(ttlSeconds, expiresAtMillis)) {
        return error("expire time is out of range");
    }

    const std::string expiresAtMillisText = std::to_string(expiresAtMillis);
    const std::vector<std::string_view> walCommand{
        "EXPIREAT_MS",
        std::string_view(keyString.data(), keyString.size()),
        std::string_view(expiresAtMillisText.data(), expiresAtMillisText.size())
    };

    if (!walLogger_.append(walCommand)) {
        return error("failed to write WAL");
    }

    iterator->second.expiresAt = expiresAt;
    return integer(1);
}

std::string Store::handleTtl(const std::vector<RespValue>& arguments)
{
    if (arguments.size() != 2) {
        return error("wrong number of arguments for 'ttl' command");
    }

    std::string_view key;
    if (!argumentText(arguments[1], key)) {
        return error("TTL key must be a non-null bulk string");
    }

    const std::string keyString(key);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator == data_.end()) {
        return integer(-2);
    }

    const Clock::time_point now = Clock::now();
    if (isExpired(iterator->second, now)) {
        eraseEntry(iterator);
        return integer(-2);
    }

    if (!iterator->second.expiresAt.has_value()) {
        return integer(-1);
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        *iterator->second.expiresAt - now);
    const std::int64_t remainingSeconds = remaining.count() < 0 ? 0 : remaining.count();
    return integer(remainingSeconds);
}

std::string Store::handleLpush(const std::vector<RespValue>& arguments)
{
    if (arguments.size() < 3) {
        return error("wrong number of arguments for 'lpush' command");
    }

    std::string_view key;
    if (!argumentText(arguments[1], key)) {
        return error("LPUSH key must be a non-null bulk string");
    }

    std::vector<std::string_view> values;
    values.reserve(arguments.size() - 2);
    for (std::size_t index = 2; index < arguments.size(); ++index) {
        std::string_view value;
        if (!argumentText(arguments[index], value)) {
            return error("LPUSH values must be non-null bulk strings");
        }

        values.push_back(value);
    }

    const std::string keyString(key);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator != data_.end()) {
        const Clock::time_point now = Clock::now();
        if (isExpired(iterator->second, now)) {
            eraseEntry(iterator);
            iterator = data_.end();
        } else if (!holdsList(iterator->second)) {
            return wrongTypeError();
        }
    }

    std::vector<std::string_view> walCommand;
    walCommand.reserve(arguments.size());
    walCommand.emplace_back("LPUSH");
    walCommand.emplace_back(keyString.data(), keyString.size());
    for (std::string_view value : values) {
        walCommand.push_back(value);
    }

    if (!walLogger_.append(walCommand)) {
        return error("failed to write WAL");
    }

    lpushEntryLocked(keyString, values);
    CacheMap::iterator updated = data_.find(keyString);
    const ListValue& list = std::get<ListValue>(updated->second.value);
    return integer(static_cast<std::int64_t>(list.size()));
}

std::string Store::handleRpop(const std::vector<RespValue>& arguments)
{
    if (arguments.size() != 2) {
        return error("wrong number of arguments for 'rpop' command");
    }

    std::string_view key;
    if (!argumentText(arguments[1], key)) {
        return error("RPOP key must be a non-null bulk string");
    }

    const std::string keyString(key);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator == data_.end()) {
        return nullBulkString();
    }

    const Clock::time_point now = Clock::now();
    if (isExpired(iterator->second, now)) {
        eraseEntry(iterator);
        return nullBulkString();
    }

    if (!holdsList(iterator->second)) {
        return wrongTypeError();
    }

    ListValue& list = std::get<ListValue>(iterator->second.value);
    if (list.empty()) {
        return nullBulkString();
    }

    const std::string value = list.back();
    const std::vector<std::string_view> walCommand{
        "RPOP",
        std::string_view(keyString.data(), keyString.size())
    };

    if (!walLogger_.append(walCommand)) {
        return error("failed to write WAL");
    }

    rpopEntryLocked(keyString);
    return bulkString(std::string_view(value.data(), value.size()));
}

std::string Store::handleLrange(const std::vector<RespValue>& arguments)
{
    if (arguments.size() != 4) {
        return error("wrong number of arguments for 'lrange' command");
    }

    std::string_view key;
    std::string_view startText;
    std::string_view stopText;
    if (!argumentText(arguments[1], key) ||
        !argumentText(arguments[2], startText) ||
        !argumentText(arguments[3], stopText)) {
        return error("LRANGE key, start, and stop must be non-null bulk strings");
    }

    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!parseInt64(startText, start) || !parseInt64(stopText, stop)) {
        return error("value is not an integer or out of range");
    }

    const std::string keyString(key);
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator == data_.end()) {
        return emptyArray();
    }

    const Clock::time_point now = Clock::now();
    if (isExpired(iterator->second, now)) {
        eraseEntry(iterator);
        return emptyArray();
    }

    if (!holdsList(iterator->second)) {
        return wrongTypeError();
    }

    touch(iterator);
    const ListValue& list = std::get<ListValue>(iterator->second.value);
    if (list.empty()) {
        return emptyArray();
    }

    const std::int64_t size = static_cast<std::int64_t>(list.size());
    if (start < 0) {
        start += size;
    }
    if (stop < 0) {
        stop += size;
    }
    if (start < 0) {
        start = 0;
    }
    if (stop < 0 || start >= size) {
        return emptyArray();
    }
    if (stop >= size) {
        stop = size - 1;
    }
    if (start > stop) {
        return emptyArray();
    }

    return arrayBulkStrings(list, static_cast<std::size_t>(start), static_cast<std::size_t>(stop));
}

void Store::recover()
{
    std::uintmax_t validByteCount = 0;

    std::ifstream input(walLogger_.path(), std::ios::binary);
    if (!input.is_open()) {
        std::ofstream createFile(walLogger_.path(), std::ios::binary | std::ios::app);
        if (!createFile.is_open()) {
            throw std::runtime_error("failed to open WAL for recovery");
        }
        return;
    }

    RespParser parser;
    std::array<char, 4096> buffer{};
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        if (bytesRead > 0) {
            parser.append(std::string_view(buffer.data(), static_cast<std::size_t>(bytesRead)));
        }

        for (;;) {
            ParseResult result = parser.parseOne();
            if (result.status == ParseStatus::Incomplete) {
                break;
            }

            if (result.status == ParseStatus::Malformed) {
                throw std::runtime_error("malformed WAL record: " + result.errorMessage);
            }

            applyRecoveredCommand(result.value);
            validByteCount += static_cast<std::uintmax_t>(result.consumedBytes);
            parser.discardConsumed(result.consumedBytes);
        }

        if (!input) {
            break;
        }
    }

    input.close();

    if (parser.bufferedBytes() > 0) {
        std::filesystem::resize_file(walLogger_.path(), validByteCount);
    }
}

void Store::applyRecoveredCommand(const RespValue& value)
{
    if (value.type != RespType::Array || value.isNull || value.elements.empty()) {
        throw std::runtime_error("WAL record must be a non-empty RESP array");
    }

    std::vector<std::string> command;
    command.reserve(value.elements.size());

    for (const RespValue& element : value.elements) {
        std::string_view argument;
        if (!argumentText(element, argument)) {
            throw std::runtime_error("WAL record arguments must be non-null bulk strings");
        }

        command.emplace_back(argument);
    }

    if (equalsAsciiInsensitive(command[0], "SET")) {
        if (command.size() != 3) {
            throw std::runtime_error("WAL SET record has invalid arity");
        }

        setEntryLocked(command[1], command[2]);
        return;
    }

    if (equalsAsciiInsensitive(command[0], "DEL")) {
        if (command.size() < 2) {
            throw std::runtime_error("WAL DEL record has invalid arity");
        }

        std::vector<std::string> keys(command.begin() + 1, command.end());
        delEntriesLocked(keys);
        return;
    }

    if (equalsAsciiInsensitive(command[0], "EXPIREAT_MS")) {
        if (command.size() != 3) {
            throw std::runtime_error("WAL EXPIREAT_MS record has invalid arity");
        }

        std::int64_t expiresAtMillis = 0;
        if (!parseInt64(command[2], expiresAtMillis)) {
            throw std::runtime_error("WAL EXPIREAT_MS value is not an integer");
        }

        expireAtMillisLocked(command[1], expiresAtMillis);
        return;
    }

    if (equalsAsciiInsensitive(command[0], "LPUSH")) {
        if (command.size() < 3) {
            throw std::runtime_error("WAL LPUSH record has invalid arity");
        }

        std::vector<std::string_view> values;
        values.reserve(command.size() - 2);
        for (std::size_t index = 2; index < command.size(); ++index) {
            values.emplace_back(command[index].data(), command[index].size());
        }

        lpushEntryLocked(command[1], values);
        return;
    }

    if (equalsAsciiInsensitive(command[0], "RPOP")) {
        if (command.size() != 2) {
            throw std::runtime_error("WAL RPOP record has invalid arity");
        }

        rpopEntryLocked(command[1]);
        return;
    }

    if (equalsAsciiInsensitive(command[0], "EXPIRE")) {
        if (command.size() != 3) {
            throw std::runtime_error("WAL EXPIRE record has invalid arity");
        }

        std::int64_t ttlSeconds = 0;
        if (!parseInt64(command[2], ttlSeconds)) {
            throw std::runtime_error("WAL EXPIRE value is not an integer");
        }

        std::int64_t expiresAtMillis = 0;
        if (ttlSeconds <= 0 || !computeExpiryMillis(ttlSeconds, expiresAtMillis)) {
            expiresAtMillis = 0;
        }

        expireAtMillisLocked(command[1], expiresAtMillis);
        return;
    }

    throw std::runtime_error("unknown WAL command");
}

void Store::setEntryLocked(std::string_view key, std::string_view value)
{
    const std::string keyString(key);
    const std::string valueString(value);

    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator != data_.end()) {
        iterator->second.value = valueString;
        iterator->second.expiresAt.reset();
        touch(iterator);
        return;
    }

    while (data_.size() >= maxKeys_) {
        const std::string& lruKey = lruList_.back();
        CacheMap::iterator lruIterator = data_.find(lruKey);
        if (lruIterator == data_.end()) {
            lruList_.pop_back();
            continue;
        }

        eraseEntry(lruIterator);
    }

    lruList_.push_front(keyString);
    try {
        CacheNode node;
        node.value = valueString;
        node.lruIterator = lruList_.begin();
        node.expiresAt.reset();
        data_.emplace(*lruList_.begin(), std::move(node));
    } catch (...) {
        lruList_.pop_front();
        throw;
    }
}

void Store::lpushEntryLocked(std::string_view key, const std::vector<std::string_view>& values)
{
    const std::string keyString(key);
    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator != data_.end()) {
        if (!holdsList(iterator->second)) {
            throw std::runtime_error("WAL LPUSH against non-list key");
        }

        ListValue& list = std::get<ListValue>(iterator->second.value);
        for (std::string_view value : values) {
            list.push_front(std::string(value));
        }
        iterator->second.expiresAt.reset();
        touch(iterator);
        return;
    }

    while (data_.size() >= maxKeys_) {
        const std::string& lruKey = lruList_.back();
        CacheMap::iterator lruIterator = data_.find(lruKey);
        if (lruIterator == data_.end()) {
            lruList_.pop_back();
            continue;
        }

        eraseEntry(lruIterator);
    }

    ListValue list;
    for (std::string_view value : values) {
        list.push_front(std::string(value));
    }

    lruList_.push_front(keyString);
    try {
        CacheNode node;
        node.value = std::move(list);
        node.lruIterator = lruList_.begin();
        node.expiresAt.reset();
        data_.emplace(*lruList_.begin(), std::move(node));
    } catch (...) {
        lruList_.pop_front();
        throw;
    }
}

void Store::rpopEntryLocked(std::string_view key)
{
    const std::string keyString(key);
    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator == data_.end()) {
        return;
    }

    if (!holdsList(iterator->second)) {
        throw std::runtime_error("WAL RPOP against non-list key");
    }

    ListValue& list = std::get<ListValue>(iterator->second.value);
    if (list.empty()) {
        return;
    }

    list.pop_back();
    if (list.empty()) {
        eraseEntry(iterator);
        return;
    }

    touch(iterator);
}

void Store::delEntriesLocked(const std::vector<std::string>& keys)
{
    for (const std::string& key : keys) {
        CacheMap::iterator iterator = data_.find(key);
        if (iterator != data_.end()) {
            eraseEntry(iterator);
        }
    }
}

void Store::expireAtMillisLocked(std::string_view key, std::int64_t expiresAtMillis)
{
    const std::string keyString(key);
    CacheMap::iterator iterator = data_.find(keyString);
    if (iterator == data_.end()) {
        return;
    }

    if (expiresAtMillis <= 0) {
        eraseEntry(iterator);
        return;
    }

    const auto maxMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
        WallClock::time_point::max().time_since_epoch());
    if (expiresAtMillis > maxMillis.count()) {
        return;
    }

    const WallClock::time_point wallExpiry{std::chrono::milliseconds(expiresAtMillis)};
    const WallClock::time_point wallNow = WallClock::now();
    if (wallExpiry <= wallNow) {
        eraseEntry(iterator);
        return;
    }

    const auto remaining = std::chrono::duration_cast<Clock::duration>(wallExpiry - wallNow);
    iterator->second.expiresAt = Clock::now() + remaining;
}

void Store::touch(CacheMap::iterator iterator)
{
    // O(1): std::list::splice relinks nodes without allocating or copying keys.
    lruList_.splice(lruList_.begin(), lruList_, iterator->second.lruIterator);
    iterator->second.lruIterator = lruList_.begin();
}

void Store::eraseEntry(CacheMap::iterator iterator)
{
    lruList_.erase(iterator->second.lruIterator);
    data_.erase(iterator);
}

void Store::activeExpiryLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(expiryWakeMutex_);
        const bool shouldStop = expiryWakeCondition_.wait_for(lock, kExpiryInterval, [this]() {
            return stopExpiryThread_;
        });

        if (shouldStop) {
            return;
        }

        lock.unlock();
        expireSample();
    }
}

void Store::expireSample()
{
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    if (data_.empty()) {
        return;
    }

    static thread_local std::mt19937_64 generator(std::random_device{}());
    const Clock::time_point now = Clock::now();
    const std::size_t samples = data_.size() < kExpiryScanBudget ? data_.size() : kExpiryScanBudget;

    for (std::size_t scanned = 0; scanned < samples && !data_.empty(); ++scanned) {
        std::uniform_int_distribution<std::size_t> distribution(0, data_.bucket_count() - 1);
        const std::size_t bucket = distribution(generator);
        if (data_.bucket_size(bucket) == 0) {
            continue;
        }

        CacheMap::local_iterator localIterator = data_.begin(bucket);
        CacheMap::local_iterator localEnd = data_.end(bucket);
        while (localIterator != localEnd) {
            const std::string key = localIterator->first;
            ++localIterator;

            CacheMap::iterator iterator = data_.find(key);
            if (iterator != data_.end() && isExpired(iterator->second, now)) {
                eraseEntry(iterator);
                break;
            }
        }
    }
}

bool Store::isExpired(const CacheNode& node, Clock::time_point now) const
{
    return node.expiresAt.has_value() && now >= *node.expiresAt;
}

bool Store::parseInt64(std::string_view text, std::int64_t& output) const
{
    if (text.empty()) {
        return false;
    }

    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, output, 10);
    return result.ec == std::errc() && result.ptr == end;
}

bool Store::computeExpiryTime(Clock::time_point now,
                              std::int64_t ttlSeconds,
                              Clock::time_point& output) const
{
    if (ttlSeconds <= 0) {
        return false;
    }

    const Clock::duration remainingRange = Clock::time_point::max() - now;
    const auto maxSeconds = std::chrono::duration_cast<std::chrono::seconds>(remainingRange).count();
    if (ttlSeconds > maxSeconds) {
        return false;
    }

    output = now + std::chrono::seconds(ttlSeconds);
    return true;
}

bool Store::computeExpiryMillis(std::int64_t ttlSeconds, std::int64_t& expiresAtMillis) const
{
    if (ttlSeconds <= 0) {
        return false;
    }

    const WallClock::time_point now = WallClock::now();
    const WallClock::duration remainingRange = WallClock::time_point::max() - now;
    const auto maxSeconds = std::chrono::duration_cast<std::chrono::seconds>(remainingRange).count();
    if (ttlSeconds > maxSeconds) {
        return false;
    }

    const WallClock::time_point expiresAt = now + std::chrono::seconds(ttlSeconds);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        expiresAt.time_since_epoch());

    if (millis.count() > std::numeric_limits<std::int64_t>::max()) {
        return false;
    }

    expiresAtMillis = static_cast<std::int64_t>(millis.count());
    return true;
}

bool Store::argumentText(const RespValue& value, std::string_view& output)
{
    if (value.isNull) {
        return false;
    }

    if (value.type == RespType::BulkString) {
        output = std::string_view(value.text.data(), value.text.size());
        return true;
    }

    return false;
}

bool Store::equalsAsciiInsensitive(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }

    const auto normalize = [](char value) {
        if (value >= 'a' && value <= 'z') {
            return static_cast<char>(value - ('a' - 'A'));
        }

        return value;
    };

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (normalize(left[index]) != normalize(right[index])) {
            return false;
        }
    }

    return true;
}

bool Store::holdsString(const CacheNode& node)
{
    return std::holds_alternative<std::string>(node.value);
}

bool Store::holdsList(const CacheNode& node)
{
    return std::holds_alternative<ListValue>(node.value);
}

std::string Store::simpleString(std::string_view value)
{
    std::string response;
    response.reserve(value.size() + 3);
    response.push_back('+');
    response.append(value.data(), value.size());
    response.append("\r\n");
    return response;
}

std::string Store::error(std::string_view value)
{
    std::string response;
    response.reserve(value.size() + 7);
    response.append("-ERR ");
    response.append(value.data(), value.size());
    response.append("\r\n");
    return response;
}

std::string Store::wrongTypeError()
{
    return "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
}

std::string Store::integer(std::int64_t value)
{
    return ":" + std::to_string(value) + "\r\n";
}

std::string Store::bulkString(std::string_view value)
{
    std::string response;
    response.reserve(value.size() + 32);
    response.push_back('$');
    response.append(std::to_string(value.size()));
    response.append("\r\n");
    response.append(value.data(), value.size());
    response.append("\r\n");
    return response;
}

std::string Store::nullBulkString()
{
    return "$-1\r\n";
}

std::string Store::arrayBulkStrings(const ListValue& values, std::size_t start, std::size_t stop)
{
    const std::size_t count = stop - start + 1;
    std::size_t responseSize = 1 + decimalDigits(count) + 2;
    for (std::size_t index = start; index <= stop; ++index) {
        const std::size_t valueSize = values[index].size();
        responseSize += 1 + decimalDigits(valueSize) + 2 + valueSize + 2;
    }

    std::string response;
    response.reserve(responseSize);
    response.push_back('*');
    response.append(std::to_string(count));
    response.append("\r\n");

    for (std::size_t index = start; index <= stop; ++index) {
        const std::string& value = values[index];
        response.push_back('$');
        response.append(std::to_string(value.size()));
        response.append("\r\n");
        response.append(value.data(), value.size());
        response.append("\r\n");
    }

    return response;
}

std::string Store::emptyArray()
{
    return "*0\r\n";
}
