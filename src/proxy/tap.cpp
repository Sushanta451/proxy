// tcp-tap v1 — a transparent TCP proxy that logs both halves of a connection.
//
// connection-logger watched one end of a connection from the inside. This sits
// in the middle: it accepts a downstream connection, dials the upstream, and
// relays bytes between them with poll(2), printing every chunk along with the
// direction it travelled. v1 is a faithful mirror — it rewrites nothing. The
// fault injection in ROADMAP.md gets layered onto this pump.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

// A write to a peer that has already gone away raises SIGPIPE, which would kill
// the proxy mid-relay. The two platforms disagree about where the suppression
// belongs: BSD/macOS has a per-socket SO_NOSIGPIPE, Linux wants MSG_NOSIGNAL on
// every individual send.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags{MSG_NOSIGNAL};
#else
constexpr int kSendFlags{0};
#endif

constexpr size_t kBufferSize{4096};
constexpr size_t kPreviewBytes{64};

bool suppress_sigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int on{1};
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, static_cast<socklen_t>(sizeof(on))) == -1)
    {
        std::cerr << "setsockopt(SO_NOSIGPIPE): " << std::strerror(errno) << '\n';
        return false;
    }
#else
    (void)fd; // Linux handles this with kSendFlags on each send instead.
#endif
    return true;
}

// send() is allowed to accept fewer bytes than it was offered, so everything the
// proxy relays leaves through this loop. A short write is normal here, not an
// error: the kernel's send buffer filled up and the rest still has to go.
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

// A recv buffer is not a string: it is not NUL-terminated and it can hold any
// byte at all, including the ones that would scramble a terminal. Print it
// through a bounded view and escape everything that is not plainly printable.
void print_preview(std::string_view bytes)
{
    std::string_view head{bytes.substr(0, kPreviewBytes)};
    std::cout << '[';
    for (char raw : head)
    {
        auto byte{static_cast<unsigned char>(raw)};
        if (byte >= 0x20 && byte < 0x7f)
        {
            std::cout << static_cast<char>(byte);
        }
        else if (byte == '\n')
        {
            std::cout << "\\n";
        }
        else if (byte == '\r')
        {
            std::cout << "\\r";
        }
        else if (byte == '\t')
        {
            std::cout << "\\t";
        }
        else
        {
            std::cout << '.';
        }
    }
    std::cout << ']';
    if (bytes.size() > kPreviewBytes)
    {
        std::cout << " +" << (bytes.size() - kPreviewBytes) << " more";
    }
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

// v1 takes host:port and splits on the last colon, which is enough for IPv4 and
// hostnames. The bracketed [::1]:port form is a ROADMAP item.
bool split_upstream(const std::string& spec, std::string& host, std::string& port)
{
    size_t colon{spec.rfind(':')};
    if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size())
    {
        return false;
    }
    host = spec.substr(0, colon);
    port = spec.substr(colon + 1);
    return true;
}

// Bind to loopback, not INADDR_ANY: this is an experiment tool and nothing it
// relays should be reachable from off the machine.
int listen_on(uint16_t port)
{
    int fd{socket(AF_INET, SOCK_STREAM, 0)};
    if (fd == -1)
    {
        std::cerr << "socket: " << std::strerror(errno) << '\n';
        return -1;
    }

    int on{1};
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, static_cast<socklen_t>(sizeof(on))) == -1)
    {
        std::cerr << "setsockopt(SO_REUSEADDR): " << std::strerror(errno) << '\n';
        close(fd);
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) == -1)
    {
        std::cerr << "bind(127.0.0.1:" << port << "): " << std::strerror(errno) << '\n';
        close(fd);
        return -1;
    }

    if (listen(fd, 16) == -1)
    {
        std::cerr << "listen: " << std::strerror(errno) << '\n';
        close(fd);
        return -1;
    }

    return fd;
}

// getaddrinfo reports its failures through gai_strerror, not errno, and hands
// back a list we have to walk until something connects.
int dial(const std::string& host, const std::string& port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* candidates{nullptr};
    int status{getaddrinfo(host.c_str(), port.c_str(), &hints, &candidates)};
    if (status != 0)
    {
        std::cerr << "getaddrinfo(" << host << ':' << port << "): " << gai_strerror(status) << '\n';
        return -1;
    }

    int fd{-1};
    for (addrinfo* candidate = candidates; candidate != nullptr; candidate = candidate->ai_next)
    {
        fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd == -1)
        {
            continue;
        }
        if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
        {
            break;
        }
        close(fd); // this candidate is out; do not leak it before trying the next
        fd = -1;
    }
    freeaddrinfo(candidates);

    if (fd == -1)
    {
        std::cerr << "connect(" << host << ':' << port << "): " << std::strerror(errno) << '\n';
    }
    return fd;
}

// Relay until both directions have closed. The two halves are tracked
// separately because TCP lets one end stop writing while the other keeps going:
// a client that has finished its request still needs the response to come back.
bool relay(int down, int up)
{
    char buf[kBufferSize];
    bool down_open{true};
    bool up_open{true};
    unsigned long long to_up{0};
    unsigned long long to_down{0};

    const auto pump = [&](int from, int to, bool& from_open, const char* arrow,
                          unsigned long long& relayed) -> bool
    {
        ssize_t received{recv(from, buf, sizeof(buf), 0)};

        if (received == -1)
        {
            if (errno == EINTR)
            {
                return true; // interrupted before any data arrived; poll again
            }
            std::cerr << "recv: " << std::strerror(errno) << '\n';
            return false;
        }

        if (received == 0)
        {
            // The peer finished writing. Forward the EOF instead of tearing down the
            // whole pair, so the opposite direction can still drain.
            if (shutdown(to, SHUT_WR) == -1 && errno != ENOTCONN)
            {
                std::cerr << "shutdown: " << std::strerror(errno) << '\n';
                return false;
            }
            from_open = false;
            std::cout << arrow << " eof" << std::endl;
            return true;
        }

        auto count{static_cast<size_t>(received)};
        relayed += count;
        std::cout << arrow << ' ' << count << (count == 1 ? " byte " : " bytes ");
        print_preview(std::string_view(buf, count));
        // A long-running server's stdout is block-buffered once it is redirected,
        // and the tests read this log while the proxy is still alive. Flush.
        std::cout << std::endl;

        return send_all(to, buf, count);
    };

    // poll() takes the descriptors as an array rather than a bitmap, so there is
    // no FD_SETSIZE ceiling and no set to rebuild every pass. A negative fd is
    // skipped by poll(), which is how a direction that has seen EOF drops out.
    pollfd fds[2]{{down, POLLIN, 0}, {up, POLLIN, 0}};
    constexpr short kReadable{POLLIN | POLLHUP | POLLERR};

    while (down_open || up_open)
    {
        fds[0].fd = down_open ? down : -1;
        fds[1].fd = up_open ? up : -1;

        if (poll(fds, 2, -1) == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "poll: " << std::strerror(errno) << '\n';
            return false;
        }

        // POLLHUP and POLLERR are let through to recv(), which turns them into
        // an EOF or an errno the same way it would for any other read.
        if (down_open && (fds[0].revents & kReadable) != 0 &&
            !pump(down, up, down_open, "c>u", to_up))
        {
            return false;
        }
        if (up_open && (fds[1].revents & kReadable) != 0 &&
            !pump(up, down, up_open, "u>c", to_down))
        {
            return false;
        }
        if (((fds[0].revents | fds[1].revents) & POLLNVAL) != 0)
        {
            std::cerr << "poll: descriptor not open\n";
            return false;
        }
    }

    std::cout << "closed: " << to_up << " bytes upstream, " << to_down << " bytes downstream"
              << std::endl;
    return true;
}

// Dial the upstream for an accepted downstream connection and relay to
// completion. Owns `down` and closes it on every path. A failure here is about
// this connection only; the listener is still good, so the caller decides
// whether to carry on.
bool serve_one(int down, const sockaddr_in& peer, const std::string& upstream_host,
               const std::string& upstream_port)
{
    char peer_text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &peer.sin_addr, peer_text, sizeof(peer_text)) == nullptr)
    {
        std::cerr << "inet_ntop: " << std::strerror(errno) << '\n';
        close(down);
        return false;
    }
    std::cout << "accepted " << peer_text << ':' << ntohs(peer.sin_port) << std::endl;

    int up{dial(upstream_host, upstream_port)};
    if (up == -1)
    {
        close(down);
        return false;
    }

    if (!suppress_sigpipe(down) || !suppress_sigpipe(up))
    {
        close(down);
        close(up);
        return false;
    }

    bool relayed{relay(down, up)};

    if (close(down) == -1)
    {
        std::cerr << "close(downstream): " << std::strerror(errno) << '\n';
        relayed = false;
    }
    if (close(up) == -1)
    {
        std::cerr << "close(upstream): " << std::strerror(errno) << '\n';
        relayed = false;
    }

    return relayed;
}

// A finished child stays a zombie until its parent waits for it, and a proxy
// that serves forever would accumulate one per connection. One SIGCHLD can
// stand for several exits, so reap in a loop until nothing is left.
void reap_children(int)
{
    int saved_errno{errno}; // waitpid can clobber errno under the interrupted code
    while (waitpid(-1, nullptr, WNOHANG) > 0)
    {
    }
    errno = saved_errno;
}

bool install_reaper()
{
    struct sigaction action{};
    action.sa_handler = reap_children;
    sigemptyset(&action.sa_mask);
    // SA_NOCLDSTOP: a stopped child is not a finished one. accept() already
    // retries on EINTR, so SA_RESTART is a convenience rather than a need.
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, nullptr) == -1)
    {
        std::cerr << "sigaction(SIGCHLD): " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

void usage()
{
    std::cerr << "usage: tcptap --listen <port> --upstream <host>:<port> [--once]\n"
              << "\n"
              << "Accepts one connection on 127.0.0.1:<port>, dials the upstream,\n"
              << "and relays bytes between them, logging every chunk and its\n"
              << "direction (c>u downstream-to-upstream, u>c the other way).\n"
              << "--once serves a single connection, then exits.\n";
}

} // namespace

int main(int argc, char* argv[])
{
    uint16_t listen_port{0};
    std::string upstream_host;
    std::string upstream_port;
    bool once{false};

    for (int i = 1; i < argc; ++i)
    {
        std::string_view flag{argv[i]};
        bool has_value{i + 1 < argc};

        if (flag == "--listen" && has_value)
        {
            if (!parse_port(argv[++i], listen_port))
            {
                std::cerr << "error: --listen wants a port in 1..65535\n";
                return 1;
            }
        }
        else if (flag == "--upstream" && has_value)
        {
            if (!split_upstream(argv[++i], upstream_host, upstream_port))
            {
                std::cerr << "error: --upstream wants <host>:<port>\n";
                return 1;
            }
        }
        else if (flag == "--once")
        {
            once = true;
        }
        else if (flag == "--help" || flag == "-h")
        {
            usage();
            return 0;
        }
        else
        {
            std::cerr << "error: unexpected argument: " << flag << '\n';
            usage();
            return 1;
        }
    }

    if (listen_port == 0 || upstream_host.empty())
    {
        usage();
        return 1;
    }

    int listen_fd{listen_on(listen_port)};
    if (listen_fd == -1)
    {
        return 1;
    }
    if (!once && !install_reaper())
    {
        close(listen_fd);
        return 1;
    }
    std::cout << "listening on 127.0.0.1:" << listen_port << ", upstream " << upstream_host << ':'
              << upstream_port << std::endl;

    bool ok{true};
    while (true)
    {
        sockaddr_in peer{};
        auto peer_len{static_cast<socklen_t>(sizeof(peer))};
        int down{accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len)};
        if (down == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            // An accept failure means the listener itself is in trouble; looping
            // on it would just spin, so this one ends the run.
            std::cerr << "accept: " << std::strerror(errno) << '\n';
            ok = false;
            break;
        }
        // Under --once the single connection *is* the run, so it is served in
        // this process and decides the exit status.
        if (once)
        {
            ok = serve_one(down, peer, upstream_host, upstream_port);
            break;
        }

        // Otherwise each connection gets a child, so a slow one no longer holds
        // up the next accept(). Anything still buffered in std::cout would be
        // copied into the child and printed twice, so flush before forking.
        std::cout.flush();
        pid_t child{fork()};
        if (child == -1)
        {
            // Out of processes is this connection's problem; the listener is
            // fine, so drop the connection and keep accepting.
            std::cerr << "fork: " << std::strerror(errno) << '\n';
            close(down);
            continue;
        }
        if (child == 0)
        {
            // The child never accepts, so it lets go of the listener. A failed
            // connection is reported in the child's exit status, not the run's.
            close(listen_fd);
            bool served{serve_one(down, peer, upstream_host, upstream_port)};
            // _exit skips the stdio teardown, so flush what the relay printed.
            std::cout.flush();
            _exit(served ? 0 : 1);
        }
        if (close(down) == -1)
        {
            std::cerr << "close(accepted): " << std::strerror(errno) << '\n';
        }
    }

    if (close(listen_fd) == -1)
    {
        std::cerr << "close(listener): " << std::strerror(errno) << '\n';
        ok = false;
    }
    return ok ? 0 : 1;
}
