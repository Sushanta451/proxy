// A small readiness-notification shim: register descriptors with the read and
// write interest they currently have, then wait for any of them to be ready.
//
// The relay is written against this interface rather than against poll()
// directly, so a faster platform backend can slot in behind it without
// touching the relay. On macOS and the BSDs that backend is kqueue; everywhere
// else poll() is the portable default, since it is in POSIX.
//
// Both backends implement the same three calls with the same meaning, chosen
// by #if at compile time. A backend that only compiles on one platform must
// never be the only definition of EventLoop.

#pragma once

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) ||   \
    defined(__DragonFly__)
#define TCPTAP_EVENT_KQUEUE 1
#endif

#ifdef TCPTAP_EVENT_KQUEUE
#include <sys/event.h>
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
