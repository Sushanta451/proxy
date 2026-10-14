// A small readiness-notification shim: register descriptors with the read and
// write interest they currently have, then wait for any of them to be ready.
//
// The relay is written against this interface rather than against poll()
// directly, so a faster platform backend (kqueue, epoll) can slot in behind it
// without touching the relay. poll() is the portable default: it is in POSIX
// and works everywhere the project builds.

#pragma once

#include <poll.h>

#include <cerrno>
#include <cstddef>
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

} // namespace tcptap
