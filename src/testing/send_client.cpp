// tcptap-send — the downstream client the test suite drives tcp-tap with.
//
// Connects to loopback, sends one message, half-closes so the far end sees a
// clean EOF, then reads until the reply stops. The half-close is the point: it
// exercises the proxy's ability to forward a shutdown in one direction while
// the other is still carrying data.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags{MSG_NOSIGNAL};
#else
constexpr int kSendFlags{0};
#endif

constexpr size_t kBufferSize{4096};

bool send_all(int fd, const char* data, size_t len)
{
    size_t sent{0};
    while (sent < len)
    {
        ssize_t written{send(fd, data + sent, len - sent, kSendFlags)};
        if (written == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "send: " << std::strerror(errno) << '\n';
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

bool parse_port(const char* text, uint16_t& out)
{
    char* end{nullptr};
    errno = 0;
    unsigned long value{std::strtoul(text, &end, 10)};
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > 65535)
    {
        return false;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "usage: tcptap-send <port> <message>\n";
        return 1;
    }

    uint16_t port{0};
    if (!parse_port(argv[1], port))
    {
        std::cerr << "error: port must be in 1..65535\n";
        return 1;
    }
    std::string message{argv[2]};

    int fd{socket(AF_INET, SOCK_STREAM, 0)};
    if (fd == -1)
    {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return 1;
    }

#ifdef SO_NOSIGPIPE
    int on{1};
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, static_cast<socklen_t>(sizeof(on))) == -1)
    {
        std::cerr << "setsockopt(SO_NOSIGPIPE): " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // inet_pton for parsing, never inet_addr — it cannot report a bad address.
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
    {
        std::cerr << "inet_pton: " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    auto start{std::chrono::steady_clock::now()};
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) == -1)
    {
        std::cerr << "connect(127.0.0.1:" << port << "): " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    if (!send_all(fd, message.data(), message.size()))
    {
        close(fd);
        return 1;
    }
    std::cout << "sent " << message.size() << " bytes [" << message << ']' << std::endl;

    // Half-close: we are done writing, but the reply is still coming.
    if (shutdown(fd, SHUT_WR) == -1)
    {
        std::cerr << "shutdown: " << std::strerror(errno) << '\n';
        close(fd);
        return 1;
    }

    bool ok{true};
    std::string reply;
    char buf[kBufferSize];
    while (true)
    {
        ssize_t received{recv(fd, buf, sizeof(buf), 0)};
        if (received == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "recv: " << std::strerror(errno) << '\n';
            ok = false;
            break;
        }
        if (received == 0)
        {
            break;
        }
        reply.append(buf, static_cast<size_t>(received));
    }

    if (ok)
    {
        auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)};
        std::cout << "got " << reply.size() << " bytes [" << reply << ']' << std::endl;
        std::cout << "elapsed " << elapsed.count() << " ms" << std::endl;
    }

    if (close(fd) == -1)
    {
        std::cerr << "close: " << std::strerror(errno) << '\n';
        ok = false;
    }
    return ok ? 0 : 1;
}
