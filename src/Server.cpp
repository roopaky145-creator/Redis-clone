#include "Server.h"

#include "RespParser.h"
#include "ThreadPool.h"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::string trimWindowsMessage(std::string message)
{
    while (!message.empty() &&
           (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }

    return message;
}

[[nodiscard]] std::string formatWinsockError(std::string_view operation, int errorCode)
{
    LPSTR rawMessage = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;

    const DWORD length = FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(errorCode),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&rawMessage),
        0,
        nullptr);

    std::ostringstream stream;
    stream << operation << " failed with WSA error " << errorCode;

    if (length != 0 && rawMessage != nullptr) {
        stream << ": " << trimWindowsMessage(std::string(rawMessage, length));
        LocalFree(rawMessage);
    }

    return stream.str();
}

[[nodiscard]] std::string formatAddressInfoError(std::string_view operation, int errorCode)
{
    std::ostringstream stream;
    stream << operation << " failed with getaddrinfo error " << errorCode
           << ": " << gai_strerrorA(errorCode);
    return stream.str();
}

class WinsockSession {
public:
    WinsockSession()
    {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error(formatWinsockError("WSAStartup", result));
        }
    }

    ~WinsockSession()
    {
        WSACleanup();
    }

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
};

class SocketHandle {
public:
    SocketHandle() = default;

    explicit SocketHandle(SOCKET socket) noexcept
        : socket_(socket)
    {
    }

    ~SocketHandle()
    {
        reset();
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept
        : socket_(std::exchange(other.socket_, INVALID_SOCKET))
    {
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            socket_ = std::exchange(other.socket_, INVALID_SOCKET);
        }

        return *this;
    }

    [[nodiscard]] SOCKET get() const noexcept
    {
        return socket_;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return socket_ != INVALID_SOCKET;
    }

    [[nodiscard]] SOCKET release() noexcept
    {
        return std::exchange(socket_, INVALID_SOCKET);
    }

    void reset(SOCKET socket = INVALID_SOCKET) noexcept
    {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }

        socket_ = socket;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

class AddressInfo {
public:
    explicit AddressInfo(addrinfo* value) noexcept
        : value_(value)
    {
    }

    ~AddressInfo()
    {
        if (value_ != nullptr) {
            freeaddrinfo(value_);
        }
    }

    AddressInfo(const AddressInfo&) = delete;
    AddressInfo& operator=(const AddressInfo&) = delete;

    [[nodiscard]] addrinfo* get() const noexcept
    {
        return value_;
    }

private:
    addrinfo* value_ = nullptr;
};

[[nodiscard]] SocketHandle createListeningSocket(const std::string& bindAddress, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* rawAddressInfo = nullptr;
    const std::string portText = std::to_string(port);
    const int addressResult = getaddrinfo(
        bindAddress.c_str(),
        portText.c_str(),
        &hints,
        &rawAddressInfo);

    if (addressResult != 0) {
        throw std::runtime_error(formatAddressInfoError("getaddrinfo", addressResult));
    }

    AddressInfo addressInfo(rawAddressInfo);

    SocketHandle listenSocket(socket(addressInfo.get()->ai_family,
                                     addressInfo.get()->ai_socktype,
                                     addressInfo.get()->ai_protocol));
    if (!listenSocket.valid()) {
        const int errorCode = WSAGetLastError();
        throw std::runtime_error(formatWinsockError("socket", errorCode));
    }

    const int reuseAddress = 1;
    if (setsockopt(listenSocket.get(),
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuseAddress),
                   sizeof(reuseAddress)) == SOCKET_ERROR) {
        const int errorCode = WSAGetLastError();
        throw std::runtime_error(formatWinsockError("setsockopt(SO_REUSEADDR)", errorCode));
    }

    if (bind(listenSocket.get(),
             addressInfo.get()->ai_addr,
             static_cast<int>(addressInfo.get()->ai_addrlen)) == SOCKET_ERROR) {
        const int errorCode = WSAGetLastError();
        throw std::runtime_error(formatWinsockError("bind", errorCode));
    }

    if (listen(listenSocket.get(), SOMAXCONN) == SOCKET_ERROR) {
        const int errorCode = WSAGetLastError();
        throw std::runtime_error(formatWinsockError("listen", errorCode));
    }

    return listenSocket;
}

[[nodiscard]] std::string parserErrorResponse(std::string_view message)
{
    std::string response;
    response.reserve(message.size() + 7);
    response.append("-ERR ");
    response.append(message.data(), message.size());
    response.append("\r\n");
    return response;
}

[[nodiscard]] std::string commandErrorResponse(std::string_view message)
{
    std::string response;
    response.reserve(message.size() + 7);
    response.append("-ERR ");
    response.append(message.data(), message.size());
    response.append("\r\n");
    return response;
}

[[nodiscard]] std::string integerResponse(std::size_t value)
{
    return ":" + std::to_string(value) + "\r\n";
}

[[nodiscard]] std::string simpleStringResponse(std::string_view value)
{
    std::string response;
    response.reserve(value.size() + 3);
    response.push_back('+');
    response.append(value.data(), value.size());
    response.append("\r\n");
    return response;
}

[[nodiscard]] std::string bulkStringResponse(std::string_view value)
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

[[nodiscard]] std::string pubSubPingResponse(const std::vector<std::string_view>& arguments)
{
    if (arguments.size() == 1) {
        return simpleStringResponse("PONG");
    }

    if (arguments.size() == 2) {
        return bulkStringResponse(arguments[1]);
    }

    return commandErrorResponse("wrong number of arguments for 'ping' command");
}

[[nodiscard]] bool equalsAsciiInsensitive(std::string_view left, std::string_view right)
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

[[nodiscard]] bool extractBulkArguments(const RespValue& request,
                                        std::vector<std::string_view>& arguments)
{
    if (request.type != RespType::Array || request.isNull) {
        return false;
    }

    arguments.clear();
    arguments.reserve(request.elements.size());
    for (const RespValue& element : request.elements) {
        if (element.type != RespType::BulkString || element.isNull) {
            return false;
        }

        arguments.emplace_back(element.text.data(), element.text.size());
    }

    return !arguments.empty();
}

[[nodiscard]] std::string subscribeResponse(std::string_view channel, std::size_t count)
{
    std::string response;
    response.reserve(channel.size() + 64);
    response.append("*3\r\n");
    response.append("$9\r\nsubscribe\r\n");
    response.push_back('$');
    response.append(std::to_string(channel.size()));
    response.append("\r\n");
    response.append(channel.data(), channel.size());
    response.append("\r\n:");
    response.append(std::to_string(count));
    response.append("\r\n");
    return response;
}

constexpr std::string_view kPubSubModeError =
    "only (P)SUBSCRIBE / (P)UNSUBSCRIBE / PING / QUIT allowed in this context";

[[nodiscard]] bool sendAll(SOCKET socket, std::string_view bytes)
{
    std::size_t totalSent = 0;
    while (totalSent < bytes.size()) {
        const std::size_t remaining = bytes.size() - totalSent;
        const int chunkSize = static_cast<int>(
            remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())
                ? static_cast<std::size_t>(std::numeric_limits<int>::max())
                : remaining);

        const int sent = send(socket, bytes.data() + totalSent, chunkSize, 0);
        if (sent == SOCKET_ERROR) {
            const int errorCode = WSAGetLastError();
            std::cerr << formatWinsockError("send", errorCode) << '\n';
            return false;
        }

        if (sent == 0) {
            std::cerr << "send returned 0 bytes; closing client connection\n";
            return false;
        }

        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}

class PubSubCleanup {
public:
    PubSubCleanup(PubSubManager& pubSubManager, SOCKET socket) noexcept
        : pubSubManager_(pubSubManager),
          socket_(socket)
    {
    }

    ~PubSubCleanup()
    {
        pubSubManager_.unsubscribeSocket(socket_);
    }

    PubSubCleanup(const PubSubCleanup&) = delete;
    PubSubCleanup& operator=(const PubSubCleanup&) = delete;

private:
    PubSubManager& pubSubManager_;
    SOCKET socket_;
};

void handleClient(SOCKET clientSocket, ICommandHandler& commandHandler, PubSubManager& pubSubManager)
{
    PubSubCleanup cleanup(pubSubManager, clientSocket);
    RespParser parser;
    std::array<char, 4096> receiveBuffer{};
    bool pubSubMode = false;

    for (;;) {
        const int received = recv(clientSocket,
                                  receiveBuffer.data(),
                                  static_cast<int>(receiveBuffer.size()),
                                  0);

        if (received == 0) {
            return;
        }

        if (received == SOCKET_ERROR) {
            const int errorCode = WSAGetLastError();
            std::cerr << formatWinsockError("recv", errorCode) << '\n';
            return;
        }

        try {
            parser.append(std::string_view(receiveBuffer.data(), static_cast<std::size_t>(received)));
        } catch (const RespParseException& exception) {
            const std::string response = parserErrorResponse(exception.what());
            (void)sendAll(clientSocket, response);
            return;
        }

        for (;;) {
            ParseResult result = parser.parseOne();
            if (result.status == ParseStatus::Incomplete) {
                break;
            }

            if (result.status == ParseStatus::Malformed) {
                const std::string response = parserErrorResponse(result.errorMessage);
                (void)sendAll(clientSocket, response);
                return;
            }

            std::vector<std::string_view> arguments;
            const bool hasBulkArguments = extractBulkArguments(result.value, arguments);
            std::string response;
            bool closeAfterResponse = false;

            if (hasBulkArguments && equalsAsciiInsensitive(arguments[0], "SUBSCRIBE")) {
                if (arguments.size() != 2) {
                    response = commandErrorResponse("wrong number of arguments for 'subscribe' command");
                } else {
                    const std::size_t subscriptionCount = pubSubManager.subscribe(clientSocket, arguments[1]);
                    pubSubMode = true;
                    response = subscribeResponse(arguments[1], subscriptionCount);
                }
            } else if (pubSubMode) {
                if (hasBulkArguments && equalsAsciiInsensitive(arguments[0], "PING")) {
                    response = pubSubPingResponse(arguments);
                } else if (hasBulkArguments && equalsAsciiInsensitive(arguments[0], "QUIT")) {
                    if (arguments.size() == 1) {
                        response = simpleStringResponse("OK");
                        closeAfterResponse = true;
                    } else {
                        response = commandErrorResponse("wrong number of arguments for 'quit' command");
                    }
                } else {
                    response = commandErrorResponse(kPubSubModeError);
                }
            } else if (hasBulkArguments && equalsAsciiInsensitive(arguments[0], "PUBLISH")) {
                if (arguments.size() != 3) {
                    response = commandErrorResponse("wrong number of arguments for 'publish' command");
                } else {
                    response = integerResponse(pubSubManager.publish(arguments[1], arguments[2]));
                }
            } else {
                response = commandHandler.handleCommand(result.value);
            }

            const bool sent = pubSubMode
                ? pubSubManager.sendToSubscribedSocket(clientSocket, response)
                : sendAll(clientSocket, response);
            if (!sent) {
                return;
            }

            parser.discardConsumed(result.consumedBytes);
            if (closeAfterResponse) {
                return;
            }
        }
    }
}

constexpr std::size_t kDefaultWorkerThreads = 128;

} // namespace

Server::Server(ICommandHandler& commandHandler, std::string bindAddress, std::uint16_t port)
    : commandHandler_(commandHandler),
      bindAddress_(std::move(bindAddress)),
      port_(port)
{
}

void Server::run()
{
    WinsockSession winsock;
    SocketHandle listenSocket = createListeningSocket(bindAddress_, port_);
    ThreadPool threadPool(kDefaultWorkerThreads);

    std::cout << "Listening on " << bindAddress_ << ':' << port_
              << " with " << kDefaultWorkerThreads << " worker threads\n";

    for (;;) {
        sockaddr_storage clientAddress{};
        int clientAddressLength = static_cast<int>(sizeof(clientAddress));
        SocketHandle clientSocket(accept(listenSocket.get(),
                                         reinterpret_cast<sockaddr*>(&clientAddress),
                                         &clientAddressLength));

        if (!clientSocket.valid()) {
            const int errorCode = WSAGetLastError();
            std::cerr << formatWinsockError("accept", errorCode) << '\n';
            continue;
        }

        const SOCKET acceptedSocket = clientSocket.get();

        try {
            threadPool.enqueue([acceptedSocket, this]() {
                SocketHandle ownedClientSocket(acceptedSocket);
                handleClient(ownedClientSocket.get(), commandHandler_, pubSubManager_);
            });
            (void)clientSocket.release();
        } catch (const std::exception& exception) {
            std::cerr << "failed to enqueue client task: " << exception.what() << '\n';
        }
    }
}
