// tcp-tap v1 — a transparent TCP proxy that logs both halves of a connection.
//
// connection-logger watched one end of a connection from the inside. This sits
// in the middle: it accepts a downstream connection, dials the upstream, and
// relays bytes between them through a small event-loop shim, printing every chunk along with the
// direction it travelled. v1 is a faithful mirror — it rewrites nothing. The
// fault injection in ROADMAP.md gets layered onto this pump.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "event_loop.h"

namespace
{

using tcptap::EventLoop;
using tcptap::kWantRead;
using tcptap::kWantWrite;
using tcptap::Ready;

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
// How much one direction may hold for a far side that is not keeping up.
constexpr size_t kHighWaterBytes{64 * 1024};

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

// From here on the relay never wants a call to block: a recv or send that would
// have to wait reports EAGAIN instead, and the event loop (event_loop.h) decides
// when to try the call again. It waits on each descriptor's kWantRead/kWantWrite
// interest and reports a Ready entry once the backend says the call would go
// through, whether that backend is kqueue, epoll, or the portable fallback
// everywhere else. Read the existing flags first so setting O_NONBLOCK does
// not clear the others.
bool set_nonblocking(int fd)
{
    int flags{fcntl(fd, F_GETFL)};
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        std::cerr << "fcntl(O_NONBLOCK): " << std::strerror(errno) << '\n';
        return false;
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

// A seed is any 32-bit unsigned value. strtoul would quietly accept "-1" and
// leading spaces, so the first character has to be a digit.
bool parse_seed(const char* text, std::uint32_t& out)
{
    char* end{nullptr};
    errno = 0;
    unsigned long value{std::strtoul(text, &end, 10)};
    if (*text < '0' || *text > '9' || errno != 0 || *end != '\0' || value > 0xffffffffUL)
    {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
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

// One direction of the relay: bytes read from `from` and written to `to`.
// `pending` holds what has been read but not yet accepted by `to`, up to
// kHighWaterBytes. A send that takes only part of a chunk leaves the rest
// here rather than spinning on it, and the event loop says when `to` has
// room again: once a `Ready` entry for `to` comes back writable, the next
// flush() picks the pending bytes back up where it left off.
struct Direction
{
    Direction(int from_fd, int to_fd, const char* label) : from{from_fd}, to{to_fd}, arrow{label}
    {
    }

    int from;
    int to;
    const char* arrow;
    bool reading{true}; // `from` has not sent EOF yet
    bool shut{false};   // EOF has been forwarded to `to`
    std::string pending;
    unsigned long long relayed{0};
};

// Push as much of `pending` as `to` will take without blocking. Once the source
// has finished and everything it sent has gone out, forward its EOF.
bool flush(Direction& d)
{
    while (!d.pending.empty())
    {
        ssize_t written{send(d.to, d.pending.data(), d.pending.size(), kSendFlags)};
        if (written == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return true; // the rest waits for POLLOUT
            }
            std::cerr << "send: " << std::strerror(errno) << '\n';
            return false;
        }
        d.pending.erase(0, static_cast<size_t>(written));
    }

    // Forward the EOF instead of tearing down the whole pair, so the opposite
    // direction can still drain. ENOTCONN means the peer got there first.
    if (!d.reading && !d.shut)
    {
        if (shutdown(d.to, SHUT_WR) == -1 && errno != ENOTCONN)
        {
            std::cerr << "shutdown: " << std::strerror(errno) << '\n';
            return false;
        }
        d.shut = true;
    }
    return true;
}

bool read_side(Direction& d)
{
    char buf[kBufferSize];
    ssize_t received{recv(d.from, buf, sizeof(buf), 0)};

    if (received == -1)
    {
        // EINTR: interrupted before any data arrived. EAGAIN: on a non-blocking
        // socket, a fourth outcome distinct from an error and from an EOF; the
        // event loop's readable Ready entry can be stale by the time recv
        // actually runs, since another event on the same wait() may have
        // already drained what was there. Either way, loop back and wait
        // again rather than treating it as a failure.
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return true;
        }
        std::cerr << "recv: " << std::strerror(errno) << '\n';
        return false;
    }

    if (received == 0)
    {
        d.reading = false;
        std::cout << d.arrow << " eof" << std::endl;
        return flush(d);
    }

    auto count{static_cast<size_t>(received)};
    d.relayed += count;
    std::cout << d.arrow << ' ' << count << (count == 1 ? " byte " : " bytes ");
    print_preview(std::string_view(buf, count));
    // A long-running server's stdout is block-buffered once it is redirected,
    // and the tests read this log while the proxy is still alive. Flush.
    std::cout << std::endl;

    d.pending.append(buf, count);
    return flush(d);
}

// Whether to watch `d.from` for more input. Reading while a little is still
// queued keeps both sockets busy, but past the high-water mark the source is
// left unread: its bytes back up in the kernel, TCP's window closes, and a slow
// reader ends up slowing the sender instead of growing this buffer without limit.
// One recv can overshoot the mark by at most a buffer's worth.
bool wants_read(const Direction& d)
{
    return d.reading && d.pending.size() < kHighWaterBytes;
}

// Relay until both directions have closed. The two halves are tracked
// separately because TCP lets one end stop writing while the other keeps going:
// a client that has finished its request still needs the response to come back.
bool relay(int down, int up, std::uint32_t seed)
{
    // Every random decision a fault makes comes from this one generator, so a
    // run is reproduced exactly by passing the logged seed back with --seed.
    // Each connection starts from the same seed. Nothing draws from it yet; the
    // faults on the roadmap will.
    [[maybe_unused]] std::mt19937 rng{seed};

    Direction to_upstream{down, up, "c>u"};
    Direction to_downstream{up, down, "u>c"};

    // A side is watched for input on behalf of the direction it feeds, and for
    // room on behalf of the direction that writes to it.
    const auto interest = [](const Direction& feeds, const Direction& drains)
    { return (wants_read(feeds) ? kWantRead : 0U) | (drains.pending.empty() ? 0U : kWantWrite); };

    EventLoop loop;
    if (!loop.add(down, 0) || !loop.add(up, 0))
    {
        std::cerr << "event loop: " << std::strerror(errno) << '\n';
        return false;
    }

    std::vector<Ready> ready;
    while (!to_upstream.shut || !to_downstream.shut)
    {
        if (!loop.modify(down, interest(to_upstream, to_downstream)) ||
            !loop.modify(up, interest(to_downstream, to_upstream)) || !loop.wait(ready))
        {
            std::cerr << "event loop: " << std::strerror(errno) << '\n';
            return false;
        }

        for (const Ready& r : ready)
        {
            // Each side drains what is queued for it before it is read from.
            Direction& feeds{r.fd == down ? to_upstream : to_downstream};
            Direction& drains{r.fd == down ? to_downstream : to_upstream};
            if (r.writable && !drains.pending.empty() && !flush(drains))
            {
                return false;
            }
            // epoll reports EPOLLERR/EPOLLHUP even at zero interest, so a reset
            // on a fully throttled direction needs r.writable (trouble sets it
            // too) to force the read past wants_read's backpressure gate.
            if (r.readable && (wants_read(feeds) || r.writable) && !read_side(feeds))
            {
                return false;
            }
        }
    }

    std::cout << "closed: " << to_upstream.relayed << " bytes upstream, " << to_downstream.relayed
              << " bytes downstream" << std::endl;
    return true;
}

// Dial the upstream for an accepted downstream connection and relay to
// completion. Owns `down` and closes it on every path. A failure here is about
// this connection only; the listener is still good, so the caller decides
// whether to carry on.
bool serve_one(int down, const sockaddr_in& peer, std::uint32_t seed,
               const std::string& upstream_host, const std::string& upstream_port)
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

    // dial() connects while the socket is still blocking, so only the relay
    // itself runs non-blocking.
    if (!suppress_sigpipe(down) || !suppress_sigpipe(up) || !set_nonblocking(down) ||
        !set_nonblocking(up))
    {
        close(down);
        close(up);
        return false;
    }

    bool relayed{relay(down, up, seed)};

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

// `sigaction` names both a function and a struct, so the struct needs its tag.
// The alias keeps brace init without a `struct x{}` that clang-format versions
// disagree on how to space.
using SigAction = struct sigaction;

bool install_reaper()
{
    SigAction action{};
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
    std::cerr << "usage: tcptap --listen <port> --upstream <host>:<port> [--once] [--seed N]\n"
              << "\n"
              << "Accepts one connection on 127.0.0.1:<port>, dials the upstream,\n"
              << "and relays bytes between them, logging every chunk and its\n"
              << "direction (c>u downstream-to-upstream, u>c the other way).\n"
              << "--once serves a single connection, then exits.\n"
              << "--seed N seeds the fault RNG (0..4294967295); without it one is drawn\n"
              << "at random. The seed in use is logged either way, to replay a run.\n";
}

} // namespace

int main(int argc, char* argv[])
{
    uint16_t listen_port{0};
    std::string upstream_host;
    std::string upstream_port;
    bool once{false};
    bool seeded{false};
    std::uint32_t seed{0};

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
        else if (flag == "--seed" && has_value)
        {
            if (!parse_seed(argv[++i], seed))
            {
                std::cerr << "error: --seed wants an integer in 0..4294967295\n";
                return 1;
            }
            seeded = true;
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

    // Without --seed the run is still reproducible after the fact: the drawn
    // seed is logged like a given one.
    if (!seeded)
    {
        seed = std::random_device{}();
    }
    std::cout << "seed " << seed << (seeded ? "" : " (random)") << std::endl;

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
            ok = serve_one(down, peer, seed, upstream_host, upstream_port);
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
            bool served{serve_one(down, peer, seed, upstream_host, upstream_port)};
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
