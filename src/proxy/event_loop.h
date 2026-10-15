// A small readiness-notification shim: register descriptors with the read and
// write interest they currently have, then wait for any of them to be ready.
//
// The relay is written against this interface rather than against poll()
// directly, so a faster platform backend can slot in behind it without
// touching the relay: kqueue on macOS and the BSDs, epoll on Linux, and poll()
// everywhere else as the portable default, since it is in POSIX.
//
// Every backend implements the same three calls with the same meaning, chosen
// by #if at compile time. Defining TCPTAP_EVENT_POLL (CMake's
// -DTCPTAP_FORCE_POLL=ON) selects poll() on any platform, so the fallback keeps
// getting built and tested on machines that would never pick it on their own.

#pragma once

#if defined(TCPTAP_EVENT_POLL)
// forced: the portable fallback, whatever the platform
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
#define TCPTAP_EVENT_KQUEUE 1
#elif defined(__linux__)
#define TCPTAP_EVENT_EPOLL 1
#endif

#if defined(TCPTAP_EVENT_KQUEUE)
#include <sys/event.h>
#include <unistd.h>
#elif defined(TCPTAP_EVENT_EPOLL)
#include <sys/epoll.h>
#include <unistd.h>
#else
#include <poll.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace tcptap
{

constexpr unsigned kWantRead{1U << 0};
constexpr unsigned kWantWrite{1U << 1};

// What one wait() found for one descriptor. A hang-up or a pending socket
// error is reported as both readable and writable, so that the next recv() or
// send() surfaces it as an EOF or an errno like any other call would.
struct Ready
{
    int fd;
    bool readable;
    bool writable;
};

#ifdef TCPTAP_EVENT_KQUEUE

// kqueue keeps the interest set in the kernel, so a wait does not hand the whole
// list over again the way poll() does. Each descriptor gets a read and a write
// filter, enabled or disabled to match its interest.
class EventLoop
{
  public:
    EventLoop() : kq_{kqueue()}
    {
    }
    ~EventLoop()
    {
        if (kq_ != -1 && close(kq_) == -1)
        {
            std::cerr << "close(kqueue): " << std::strerror(errno) << '\n';
        }
    }
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool add(int fd, unsigned interest)
    {
        if (kq_ == -1)
        {
            return false; // errno is still kqueue()'s
        }
        // An impossible interest forces the first modify() to register both
        // filters with the kernel.
        watched_.push_back(Watched{fd, ~0U});
        return modify(fd, interest);
    }

    bool modify(int fd, unsigned interest)
    {
        for (Watched& w : watched_)
        {
            if (w.fd != fd)
            {
                continue;
            }
            if (w.interest == interest)
            {
                return true; // the relay asks every pass; skip the syscall
            }
            struct kevent changes[2];
            filter(changes[0], fd, EVFILT_READ, (interest & kWantRead) != 0);
            filter(changes[1], fd, EVFILT_WRITE, (interest & kWantWrite) != 0);
            if (kevent(kq_, changes, 2, nullptr, 0, nullptr) == -1)
            {
                return false;
            }
            w.interest = interest;
            return true;
        }
        errno = ENOENT;
        return false;
    }

    // Same contract as the poll() backend. kqueue reports read and write
    // readiness as separate events, so one descriptor can appear twice.
    bool wait(std::vector<Ready>& out)
    {
        out.clear();
        struct kevent events[8];
        int count{kevent(kq_, nullptr, 0, events, 8, nullptr)};
        if (count == -1)
        {
            return errno == EINTR;
        }
        for (int i = 0; i < count; ++i)
        {
            const struct kevent& ev{events[i]};
            if ((ev.flags & EV_ERROR) != 0)
            {
                errno = static_cast<int>(ev.data);
                return false;
            }
            // EV_EOF is kqueue's hang-up: report it both ways, like POLLHUP.
            bool hung_up{(ev.flags & EV_EOF) != 0};
            out.push_back(Ready{static_cast<int>(ev.ident), ev.filter == EVFILT_READ || hung_up,
                                ev.filter == EVFILT_WRITE || hung_up});
        }
        return true;
    }

  private:
    struct Watched
    {
        int fd;
        unsigned interest;
    };

    static void filter(struct kevent& change, int fd, short which, bool on)
    {
        EV_SET(&change, static_cast<uintptr_t>(fd), which,
               static_cast<unsigned short>(EV_ADD | (on ? EV_ENABLE : EV_DISABLE)), 0, 0, nullptr);
    }

    int kq_;
    std::vector<Watched> watched_;
};

#elif defined(TCPTAP_EVENT_EPOLL)

// epoll, like kqueue, keeps the interest set in the kernel. Unlike kqueue it has
// one registration per descriptor carrying both interests, and a descriptor with
// none is left registered with an empty mask rather than removed.
class EventLoop
{
  public:
    EventLoop() : ep_{epoll_create1(EPOLL_CLOEXEC)}
    {
    }
    ~EventLoop()
    {
        if (ep_ != -1 && close(ep_) == -1)
        {
            std::cerr << "close(epoll): " << std::strerror(errno) << '\n';
        }
    }
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool add(int fd, unsigned interest)
    {
        if (ep_ == -1)
        {
            return false; // errno is still epoll_create1()'s
        }
        epoll_event ev{mask(interest), {}};
        ev.data.fd = fd;
        if (epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) == -1)
        {
            return false;
        }
        watched_.push_back(Watched{fd, interest});
        return true;
    }

    bool modify(int fd, unsigned interest)
    {
        for (Watched& w : watched_)
        {
            if (w.fd != fd)
            {
                continue;
            }
            if (w.interest == interest)
            {
                return true; // the relay asks every pass; skip the syscall
            }
            epoll_event ev{mask(interest), {}};
            ev.data.fd = fd;
            if (epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev) == -1)
            {
                return false;
            }
            w.interest = interest;
            return true;
        }
        errno = ENOENT;
        return false;
    }

    // Same contract as the other backends. EPOLLHUP and EPOLLERR are reported
    // even with an empty mask, and count as both readable and writable.
    bool wait(std::vector<Ready>& out)
    {
        out.clear();
        epoll_event events[8];
        int count{epoll_wait(ep_, events, 8, -1)};
        if (count == -1)
        {
            return errno == EINTR;
        }
        for (int i = 0; i < count; ++i)
        {
            std::uint32_t got{events[i].events};
            bool trouble{(got & (EPOLLHUP | EPOLLERR)) != 0};
            out.push_back(Ready{events[i].data.fd, (got & EPOLLIN) != 0 || trouble,
                                (got & EPOLLOUT) != 0 || trouble});
        }
        return true;
    }

  private:
    struct Watched
    {
        int fd;
        unsigned interest;
    };

    static std::uint32_t mask(unsigned interest)
    {
        return ((interest & kWantRead) != 0 ? std::uint32_t{EPOLLIN} : 0U) |
               ((interest & kWantWrite) != 0 ? std::uint32_t{EPOLLOUT} : 0U);
    }

    int ep_;
    std::vector<Watched> watched_;
};

#else // poll()

class EventLoop
{
  public:
    // Start watching `fd` with the given kWantRead/kWantWrite interest.
    bool add(int fd, unsigned interest)
    {
        fds_.push_back(pollfd{fd, 0, 0});
        return modify(fd, interest);
    }

    // Change what `fd` is watched for. No interest at all leaves it registered
    // but skipped: poll() ignores a negative fd, so it is stored negated.
    bool modify(int fd, unsigned interest)
    {
        for (pollfd& p : fds_)
        {
            if (p.fd == fd || p.fd == -fd - 1)
            {
                p.events = static_cast<short>(((interest & kWantRead) != 0 ? POLLIN : 0) |
                                              ((interest & kWantWrite) != 0 ? POLLOUT : 0));
                p.fd = p.events == 0 ? -fd - 1 : fd;
                return true;
            }
        }
        errno = ENOENT;
        return false;
    }

    // Block until at least one descriptor is ready, then report every ready one
    // into `out`. Returns false with errno set on failure; EINTR is not one, it
    // just reports nothing so the caller goes round again.
    bool wait(std::vector<Ready>& out)
    {
        out.clear();
        if (poll(fds_.data(), static_cast<nfds_t>(fds_.size()), -1) == -1)
        {
            return errno == EINTR;
        }
        for (const pollfd& p : fds_)
        {
            if ((p.revents & POLLNVAL) != 0)
            {
                errno = EBADF;
                return false;
            }
            constexpr short kTrouble{POLLHUP | POLLERR};
            bool readable{(p.revents & (POLLIN | kTrouble)) != 0};
            bool writable{(p.revents & (POLLOUT | kTrouble)) != 0};
            if (readable || writable)
            {
                out.push_back(Ready{p.fd, readable, writable});
            }
        }
        return true;
    }

  private:
    std::vector<pollfd> fds_;
};

#endif

} // namespace tcptap
