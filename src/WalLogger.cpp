#include "WalLogger.h"

#include <ios>
#include <limits>
#include <stdexcept>
#include <utility>

WalLogger::WalLogger(std::string path)
    : path_(std::move(path))
{
}

WalLogger::~WalLogger()
{
    running_.store(false, std::memory_order_release);
    flushCondition_.notify_all();

    if (flushThread_.joinable()) {
        flushThread_.join();
    }

    flushToDisk();
}

void WalLogger::start()
{
    if (running_.load(std::memory_order_acquire) || flushThread_.joinable()) {
        throw std::logic_error("WalLogger already started");
    }

    stream_.open(path_, std::ios::binary | std::ios::app);
    writeHealthy_.store(stream_.is_open() && stream_.good(), std::memory_order_release);
    running_.store(true, std::memory_order_release);

    flushThread_ = std::thread([this]() {
        flushLoop();
    });
}

bool WalLogger::append(const std::vector<std::string_view>& command)
{
    if (!running_.load(std::memory_order_acquire) ||
        !writeHealthy_.load(std::memory_order_acquire)) {
        return false;
    }

    std::string serialized = serializeCommand(command);
    if (serialized.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        writeBuffer_.push_back(std::move(serialized));
    }

    return true;
}

const std::string& WalLogger::path() const noexcept
{
    return path_;
}

void WalLogger::flushLoop()
{
    while (running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(flushWakeMutex_);
        flushCondition_.wait_for(lock, kFlushInterval, [this]() {
            return !running_.load(std::memory_order_acquire);
        });
        lock.unlock();

        flushToDisk();
    }
}

void WalLogger::flushToDisk()
{
    std::vector<std::string> localBuffer;

    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        if (writeBuffer_.empty()) {
            return;
        }

        std::swap(localBuffer, writeBuffer_);
    }

    if (!stream_.is_open() || !stream_.good()) {
        writeHealthy_.store(false, std::memory_order_release);
        return;
    }

    for (const std::string& record : localBuffer) {
        stream_.write(record.data(), static_cast<std::streamsize>(record.size()));
        if (!stream_.good()) {
            writeHealthy_.store(false, std::memory_order_release);
            return;
        }
    }

    stream_.flush();
    if (!stream_.good()) {
        writeHealthy_.store(false, std::memory_order_release);
    }
}

std::string WalLogger::serializeCommand(const std::vector<std::string_view>& command)
{
    std::string output;
    output.reserve(32);
    output.push_back('*');
    output.append(std::to_string(command.size()));
    output.append("\r\n");

    for (std::string_view argument : command) {
        appendBulkString(output, argument);
    }

    return output;
}

void WalLogger::appendBulkString(std::string& output, std::string_view value)
{
    output.push_back('$');
    output.append(std::to_string(value.size()));
    output.append("\r\n");
    output.append(value.data(), value.size());
    output.append("\r\n");
}
