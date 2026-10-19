// tcptap-echo — the upstream the test suite points tcp-tap at.
//
// Accepts one connection on loopback, echoes every chunk straight back, and
// exits when the peer stops writing. Deliberately boring: when a test fails we
// want to be looking at the proxy, not at the thing behind it.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
    if (argc != 2)
    {
        std::cerr << "usage: tcptap-echo <port>\n";
        return 1;
    }

    uint16_t port{0};
    if (!parse_port(argv[1], port))
    {
        std::cerr << "error: port must be in 1..65535\n";
        return 1;
    }

    int listen_fd{socket(AF_INET, SOCK_STREAM, 0)};
    if (listen_fd == -1)
    {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return 1;
    }

    int on{1};
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, static_cast<socklen_t>(sizeof(on))) ==
        -1)
    {
        std::cerr << "setsockopt(SO_REUSEADDR): " << std::strerror(errno) << '\n';
        close(listen_fd);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) ==
        -1)
    {
        std::cerr << "bind(127.0.0.1:" << port << "): " << std::strerror(errno) << '\n';
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 1) == -1)
    {
        std::cerr << "listen: " << std::strerror(errno) << '\n';
        close(listen_fd);
        return 1;
    }

    // The smoke test waits for this line before it starts the proxy, and stdout
    // is block-buffered once redirected to a file, so it has to be flushed now.
    std::cout << "echo listening on 127.0.0.1:" << port << std::endl;

    int conn{accept(listen_fd, nullptr, nullptr)};
    if (conn == -1)
    {
        std::cerr << "accept: " << std::strerror(errno) << '\n';
        close(listen_fd);
        return 1;
    }
    bool ok{true};
    if (close(listen_fd) == -1)
    {
        std::cerr << "close(listener): " << std::strerror(errno) << '\n';
        ok = false;
    }

    char buf[kBufferSize];
    while (true)
    {
        ssize_t received{recv(conn, buf, sizeof(buf), 0)};
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
            std::cout << "echo saw eof" << std::endl;
            break;
        }

        auto count{static_cast<size_t>(received)};
        std::cout << "echo " << count << " bytes " << std::string_view(buf, count) << std::endl;
        if (!send_all(conn, buf, count))
        {
            ok = false;
            break;
        }
    }

    if (close(conn) == -1)
    {
        std::cerr << "close: " << std::strerror(errno) << '\n';
        ok = false;
    }
    return ok ? 0 : 1;
}
