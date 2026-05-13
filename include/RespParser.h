#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

enum class RespType {
    SimpleString,
    Error,
    Integer,
    BulkString,
    Array
};

struct RespValue {
    RespType type = RespType::SimpleString;
    bool isNull = false;
    std::string text;
    std::int64_t integer = 0;
    std::vector<RespValue> elements;
};

class RespParseException final : public std::runtime_error {
public:
    explicit RespParseException(const std::string& message);
};

enum class ParseStatus {
    Complete,
    Incomplete,
    Malformed
};

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;
    RespValue value;
    std::size_t consumedBytes = 0;
    std::string errorMessage;

    [[nodiscard]] static ParseResult complete(RespValue value, std::size_t consumedBytes);
    [[nodiscard]] static ParseResult incomplete();
    [[nodiscard]] static ParseResult malformed(std::string errorMessage);
};

class RespParser {
public:
    RespParser() = default;

    // Complexity: amortized O(n) in appended bytes. The bytes are appended once
    // to the connection buffer; field scanning later uses string_view windows.
    // Throws RespParseException if the per-connection buffer exceeds the hard
    // cap, preventing infinite-line or incomplete-frame OOM attacks.
    void append(std::string_view bytes);

    // Complexity: O(newly_completed_frame_bytes) amortized for arrays. Partial
    // array progress is retained, so a fragmented N-element array resumes at the
    // stalled element instead of reparsing elements 0..N on every append.
    [[nodiscard]] ParseResult parseOne();

    // Complexity: usually O(1). Occasionally O(remaining_buffer_size) when the
    // parser compacts already-consumed bytes to keep long-lived clients bounded.
    void discardConsumed(std::size_t consumedBytes);

    [[nodiscard]] std::size_t bufferedBytes() const noexcept;

private:
    struct InternalParseResult {
        ParseStatus status = ParseStatus::Incomplete;
        RespValue value;
        std::size_t nextOffset = 0;
        bool startedArray = false;
        std::size_t arrayLength = 0;
        std::string errorMessage;
    };

    struct ArrayFrame {
        RespValue value;
        std::size_t expectedElements = 0;
        std::size_t nextOffset = 0;
    };

    [[nodiscard]] InternalParseResult parseValueToken(std::size_t offset) const;
    [[nodiscard]] InternalParseResult parseLineValue(RespType type, std::size_t offset) const;
    [[nodiscard]] InternalParseResult parseInteger(std::size_t offset) const;
    [[nodiscard]] InternalParseResult parseBulkString(std::size_t offset) const;
    [[nodiscard]] InternalParseResult parseArrayHeader(std::size_t offset) const;

    [[nodiscard]] std::size_t findLineEnd(std::size_t offset) const;
    [[nodiscard]] bool parseInt64(std::string_view text, std::int64_t& value) const;
    void startArray(std::size_t expectedElements, std::size_t nextOffset);
    void resetPartialState() noexcept;

    static constexpr std::size_t kMaxNestingDepth = 512;
    static constexpr std::size_t kMaxArrayElements = 1'048'576;
    static constexpr std::size_t kMaxBulkStringBytes = 512 * 1024 * 1024;
    static constexpr std::size_t kMaxBufferBytes = 512 * 1024 * 1024;
    static constexpr std::size_t kCompactThresholdBytes = 4096;

    std::string buffer_;
    std::size_t readOffset_ = 0;
    std::vector<ArrayFrame> arrayStack_;
};
