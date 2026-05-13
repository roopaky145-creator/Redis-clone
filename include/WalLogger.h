#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class WalLogger {
public:
    explicit WalLogger(std::string path = "redis_clone.wal");
    ~WalLogger();

    WalLogger(const WalLogger&) = delete;
    WalLogger& operator=(const WalLogger&) = delete;
    WalLogger(WalLogger&&) = delete;
    WalLogger& operator=(WalLogger&&) = delete;

    void start();

    // Complexity: O(total_command_bytes) to serialize the RESP record, with an
    // O(1) amortized mutex-protected push into the in-memory write buffer.
    // No disk I/O occurs on this caller thread.
    [[nodiscard]] bool append(const std::vector<std::string_view>& command);

    [[nodiscard]] const std::string& path() const noexcept;

private:
    void flushLoop();
    void flushToDisk();

    [[nodiscard]] static std::string serializeCommand(const std::vector<std::string_view>& command);
    static void appendBulkString(std::string& output, std::string_view value);

    static constexpr std::chrono::milliseconds kFlushInterval{100};

    std::string path_;
    std::ofstream stream_;
    std::vector<std::string> writeBuffer_;
    std::mutex bufferMutex_;
    std::condition_variable flushCondition_;
    std::mutex flushWakeMutex_;
    std::thread flushThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> writeHealthy_{true};
};
