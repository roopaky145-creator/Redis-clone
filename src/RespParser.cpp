#include "RespParser.h"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

RespParseException::RespParseException(const std::string& message)
    : std::runtime_error(message)
{
}

ParseResult ParseResult::complete(RespValue value, std::size_t consumedBytes)
{
    ParseResult result;
    result.status = ParseStatus::Complete;
    result.value = std::move(value);
    result.consumedBytes = consumedBytes;
    return result;
}

ParseResult ParseResult::incomplete()
{
    ParseResult result;
    result.status = ParseStatus::Incomplete;
    return result;
}

ParseResult ParseResult::malformed(std::string errorMessage)
{
    ParseResult result;
    result.status = ParseStatus::Malformed;
    result.errorMessage = std::move(errorMessage);
    return result;
}

void RespParser::append(std::string_view bytes)
{
    if (bytes.empty()) {
        return;
    }

    if (buffer_.size() > kMaxBufferBytes ||
        bytes.size() > kMaxBufferBytes - buffer_.size()) {
        throw RespParseException("RESP parser buffer exceeded 512 MiB hard limit");
    }

    buffer_.append(bytes.data(), bytes.size());
}

ParseResult RespParser::parseOne()
{
    if (readOffset_ >= buffer_.size()) {
        return ParseResult::incomplete();
    }

    if (arrayStack_.empty()) {
        InternalParseResult root = parseValueToken(readOffset_);

        if (root.status == ParseStatus::Incomplete) {
            return ParseResult::incomplete();
        }

        if (root.status == ParseStatus::Malformed) {
            resetPartialState();
            return ParseResult::malformed(std::move(root.errorMessage));
        }

        if (!root.startedArray) {
            return ParseResult::complete(std::move(root.value), root.nextOffset - readOffset_);
        }

        startArray(root.arrayLength, root.nextOffset);
    }

    while (!arrayStack_.empty()) {
        if (arrayStack_.size() > kMaxNestingDepth) {
            resetPartialState();
            return ParseResult::malformed("RESP nesting depth exceeded");
        }

        ArrayFrame& currentFrame = arrayStack_.back();
        if (currentFrame.value.elements.size() == currentFrame.expectedElements) {
            RespValue completedArray = std::move(currentFrame.value);
            const std::size_t completedOffset = currentFrame.nextOffset;
            arrayStack_.pop_back();

            if (arrayStack_.empty()) {
                return ParseResult::complete(std::move(completedArray), completedOffset - readOffset_);
            }

            ArrayFrame& parentFrame = arrayStack_.back();
            parentFrame.value.elements.push_back(std::move(completedArray));
            parentFrame.nextOffset = completedOffset;
            continue;
        }

        InternalParseResult element = parseValueToken(currentFrame.nextOffset);

        if (element.status == ParseStatus::Incomplete) {
            return ParseResult::incomplete();
        }

        if (element.status == ParseStatus::Malformed) {
            resetPartialState();
            return ParseResult::malformed(std::move(element.errorMessage));
        }

        if (element.startedArray) {
            startArray(element.arrayLength, element.nextOffset);
            continue;
        }

        currentFrame.value.elements.push_back(std::move(element.value));
        currentFrame.nextOffset = element.nextOffset;
    }

    return ParseResult::incomplete();
}

void RespParser::discardConsumed(std::size_t consumedBytes)
{
    if (consumedBytes == 0) {
        return;
    }

    if (!arrayStack_.empty()) {
        throw std::logic_error("RespParser::discardConsumed called while an array frame is incomplete");
    }

    const std::size_t availableBytes = buffer_.size() - readOffset_;
    if (consumedBytes > availableBytes) {
        throw std::out_of_range("RespParser::discardConsumed beyond buffered data");
    }

    readOffset_ += consumedBytes;

    if (readOffset_ == buffer_.size()) {
        buffer_.clear();
        readOffset_ = 0;
        return;
    }

    if (readOffset_ >= kCompactThresholdBytes && readOffset_ * 2 >= buffer_.size()) {
        buffer_.erase(0, readOffset_);
        readOffset_ = 0;
    }
}

std::size_t RespParser::bufferedBytes() const noexcept
{
    return buffer_.size() - readOffset_;
}

RespParser::InternalParseResult RespParser::parseValueToken(std::size_t offset) const
{
    if (offset >= buffer_.size()) {
        return {ParseStatus::Incomplete, {}, offset, false, 0, {}};
    }

    switch (buffer_[offset]) {
    case '+':
        return parseLineValue(RespType::SimpleString, offset);
    case '-':
        return parseLineValue(RespType::Error, offset);
    case ':':
        return parseInteger(offset);
    case '$':
        return parseBulkString(offset);
    case '*':
        return parseArrayHeader(offset);
    default:
        return {ParseStatus::Malformed, {}, offset, false, 0, "unknown RESP type marker"};
    }
}

RespParser::InternalParseResult RespParser::parseLineValue(RespType type, std::size_t offset) const
{
    const std::size_t lineEnd = findLineEnd(offset + 1);
    if (lineEnd == std::string::npos) {
        return {ParseStatus::Incomplete, {}, offset, false, 0, {}};
    }

    const std::string_view text(buffer_.data() + offset + 1, lineEnd - offset - 1);

    RespValue value;
    value.type = type;
    value.text.assign(text.data(), text.size());

    return {ParseStatus::Complete, std::move(value), lineEnd + 2, false, 0, {}};
}

RespParser::InternalParseResult RespParser::parseInteger(std::size_t offset) const
{
    const std::size_t lineEnd = findLineEnd(offset + 1);
    if (lineEnd == std::string::npos) {
        return {ParseStatus::Incomplete, {}, offset, false, 0, {}};
    }

    const std::string_view integerText(buffer_.data() + offset + 1, lineEnd - offset - 1);
    std::int64_t parsedInteger = 0;
    if (!parseInt64(integerText, parsedInteger)) {
        return {ParseStatus::Malformed, {}, offset, false, 0, "invalid RESP integer"};
    }

    RespValue value;
    value.type = RespType::Integer;
    value.integer = parsedInteger;

    return {ParseStatus::Complete, std::move(value), lineEnd + 2, false, 0, {}};
}

RespParser::InternalParseResult RespParser::parseBulkString(std::size_t offset) const
{
    const std::size_t lineEnd = findLineEnd(offset + 1);
    if (lineEnd == std::string::npos) {
        return {ParseStatus::Incomplete, {}, offset, false, 0, {}};
    }

    const std::string_view lengthText(buffer_.data() + offset + 1, lineEnd - offset - 1);
    std::int64_t signedLength = 0;
    if (!parseInt64(lengthText, signedLength) || signedLength < -1) {
        return {ParseStatus::Malformed, {}, offset, false, 0, "invalid bulk string length"};
    }

    RespValue value;
    value.type = RespType::BulkString;

    const std::size_t dataStart = lineEnd + 2;
    if (signedLength == -1) {
        value.isNull = true;
        return {ParseStatus::Complete, std::move(value), dataStart, false, 0, {}};
    }

    const std::uint64_t unsignedLength = static_cast<std::uint64_t>(signedLength);
    if (unsignedLength > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return {ParseStatus::Malformed, {}, offset, false, 0, "bulk string length exceeds platform limit"};
    }

    const std::size_t length = static_cast<std::size_t>(unsignedLength);
    if (length > kMaxBulkStringBytes) {
        return {ParseStatus::Malformed, {}, offset, false, 0, "bulk string length exceeds configured limit"};
    }

    if (length > buffer_.size() - dataStart) {
        return {ParseStatus::Incomplete, {}, offset, false, 0, {}};
    }

    const std::size_t dataEnd = dataStart + length;
    if (buffer_.size() - dataEnd < 2) {
        return {ParseStatus::Incomplete, {}, offset, false, 0, {}};
    }

    if (buffer_[dataEnd] != '\r' || buffer_[dataEnd + 1] != '\n') {
        return {ParseStatus::Malformed, {}, offset, false, 0, "bulk string missing terminating CRLF"};
    }

    const std::string_view text(buffer_.data() + dataStart, length);
    value.text.assign(text.data(), text.size());

    return {ParseStatus::Complete, std::move(value), dataEnd + 2, false, 0, {}};
}

RespParser::InternalParseResult RespParser::parseArrayHeader(std::size_t offset) const
{
    const std::size_t lineEnd = findLineEnd(offset + 1);
    if (lineEnd == std::string::npos) {
        return {ParseStatus::Incomplete, {}, offset, false, 0, {}};
    }

    const std::string_view lengthText(buffer_.data() + offset + 1, lineEnd - offset - 1);
    std::int64_t signedLength = 0;
    if (!parseInt64(lengthText, signedLength) || signedLength < -1) {
        return {ParseStatus::Malformed, {}, offset, false, 0, "invalid array length"};
    }

    RespValue value;
    value.type = RespType::Array;

    const std::size_t nextOffset = lineEnd + 2;
    if (signedLength == -1) {
        value.isNull = true;
        return {ParseStatus::Complete, std::move(value), nextOffset, false, 0, {}};
    }

    const std::uint64_t unsignedLength = static_cast<std::uint64_t>(signedLength);
    if (unsignedLength > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return {ParseStatus::Malformed, {}, offset, false, 0, "array length exceeds platform limit"};
    }

    const std::size_t length = static_cast<std::size_t>(unsignedLength);
    if (length > kMaxArrayElements) {
        return {ParseStatus::Malformed, {}, offset, false, 0, "array length exceeds configured limit"};
    }

    return {ParseStatus::Complete, {}, nextOffset, true, length, {}};
}

std::size_t RespParser::findLineEnd(std::size_t offset) const
{
    return buffer_.find("\r\n", offset);
}

bool RespParser::parseInt64(std::string_view text, std::int64_t& value) const
{
    if (text.empty()) {
        return false;
    }

    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value, 10);
    return result.ec == std::errc() && result.ptr == end;
}

void RespParser::startArray(std::size_t expectedElements, std::size_t nextOffset)
{
    ArrayFrame frame;
    frame.value.type = RespType::Array;
    frame.expectedElements = expectedElements;
    frame.nextOffset = nextOffset;

    const std::size_t reserveCount = expectedElements < 1024 ? expectedElements : 1024;
    frame.value.elements.reserve(reserveCount);
    arrayStack_.push_back(std::move(frame));
}

void RespParser::resetPartialState() noexcept
{
    arrayStack_.clear();
}
