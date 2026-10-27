# tcp-tap — design overview

## What it is

A transparent TCP proxy that can lie to you on purpose.

`tcp-tap` listens on a local port, accepts a connection, dials an upstream, and
relays bytes between the two — logging every chunk with the direction it
travelled. That much is a wire tap. The second half of the project is the
interesting half: it can *degrade* the link while relaying, so the programs on
either end can be tested against a network that misbehaves instead of the
perfect loopback they are usually developed against.

Delay. Jitter. Rechunking. Bandwidth caps. Connections that die mid-transfer
with a RST instead of a clean FIN. Flipped bits. All of it seeded, so a failure
you find at 3am reproduces exactly at 9am.

## Why build it on raw syscalls

Every networking library — asio, libuv, Netty, Go's `net` — is a wrapper around
the same dozen system calls. Learning the wrapper teaches you the wrapper.
Learning `bind`/`listen`/`accept`/`recv`/`send` teaches you what every one of
those libraries is doing, and why they made the choices they did.

So there are no dependencies. Not as asceticism — because the syscalls *are*
the subject.

## The three hard problems

A proxy is harder than a server, and the difficulty is specific:

**1. Two descriptors, not one.** A server reads a request and writes a
response; it is never waiting on two things at once. A proxy always is. That
forces a readiness-notification API from the very first version. v1 used
`select()`; the relay now runs on a small event-loop shim
(`src/proxy/event_loop.h`) with three backends behind it.

`select()` is gone for good rather than kept as a fallback. Its `fd_set` is a
fixed-size bitmap, so it cannot watch a descriptor numbered `FD_SETSIZE`
(usually 1024) or higher — and writing one past that limit is undefined
behaviour, not an error. A forked child holds only a handful of
descriptors, but the single event loop planned later puts every connection's
two descriptors in one process, and there that ceiling is reachable. The set
also has to be rebuilt on every pass, because `select()` overwrites it.
`poll()` takes an array of `pollfd` instead: no ceiling, inputs and results
in separate fields, and a closed direction drops out by setting its fd to
`-1`. It is in POSIX, so it is the portable floor the faster
platform-specific APIs are measured against.

The shim has three calls — `add` a descriptor, `modify` its read/write
interest, `wait` for whatever is ready — and the backend is chosen at compile
time:

| Platform | Backend | Why |
| --- | --- | --- |
| macOS, FreeBSD, NetBSD, OpenBSD, DragonFly | `kqueue` | interest lives in the kernel; read and write are separate filters |
| Linux | `epoll` | interest lives in the kernel; one registration carries both |
| anything else, or `-DTCPTAP_FORCE_POLL=ON` | `poll()` | POSIX, so it builds everywhere |

`poll()` hands the kernel the whole descriptor list on every wait; the other
two register interest once and change it only when it changes, which is what
lets them scale. All three report a hang-up or socket error as both readable
and writable, so the relay meets it in its next `recv` or `send` the same way
on every platform. CI builds and tests each one on a platform that selects it,
with `poll()` forced on Linux because nothing here picks it on its own.

**2. Half-closes must be forwarded, not swallowed.** TCP connections close one
direction at a time. When the client stops writing, the proxy gets a `recv` of
`0` — and the naive move is to tear the pair down. That is wrong, and it breaks
every protocol where the client says "I'm done asking" and then waits for a
large reply. The correct move is `shutdown(upstream, SHUT_WR)`: pass the EOF
along and keep relaying the other direction until it closes too. The EOF has
to wait its turn behind every byte that arrived ahead of it, including one
still held for a future `--delay`: forward it too soon and the relay cuts off
its own tail.

**3. Backpressure is not optional.** If the upstream reads slower than the
client writes, bytes pile up somewhere. `send()` starts returning short counts,
then `EAGAIN`. A proxy that ignores this either blocks — stalling the other
direction — or loses data. The only correct answer is a per-direction write
buffer and the discipline to stop reading from a socket whose peer cannot keep
up.

None of these three come up in a program that owns one end of one connection.
All three are unavoidable in the middle.

## Shape of the program

```
          ┌──────────┐                ┌──────────┐
 client ──┤ downstream   tcp-tap   upstream ├── server
          └──────────┘                └──────────┘
                │                          │
                └──── event loop ──────────┘
                   readiness → read → fault pipeline → write
```

One connection is a pair of descriptors plus the state attached to it: which
halves are still open, how many bytes have gone each way, and the buffered
bytes that have not yet been accepted by the far side.

Faults sit between the read and the write as an ordered pipeline. The order
matters and is fixed: **rechunk → corrupt → rate-limit → delay**. Corrupting
after rechunking means a flipped bit lands in a specific segment; delaying last
means the delay describes when bytes hit the wire, which is what a user means
by latency.

## What is deliberately out of scope

- **Not a load balancer.** No upstream pools, no health checks, no failover.
- **Not a TLS terminator.** It reads enough of the TLS handshake to recognise
  records and extract SNI for routing, and never decrypts. Relaying ciphertext
  faithfully is the whole job.
- **Not a production proxy.** It binds loopback by default and is a test
  instrument. Reach for HAProxy or Envoy for real traffic.
- **No dependencies.** If a task seems to need a library, the task has been
  misread.

## Tech stack

| Layer | Choice | Why it is worth learning |
| --- | --- | --- |
| Language | C++20 | Brace init that catches narrowing, `string_view` over raw buffers, RAII for descriptors |
| Network API | POSIX / BSD sockets | The interface every networking library in every language wraps |
| Multiplexing | ~~`select()`~~ → `poll()` → `kqueue` (BSD/macOS) + `epoll` (Linux) | The actual historical progression of how servers learned to scale, walked one step at a time |
| Protocol literacy | TCP half-close & RST, HTTP/1.1 request lines, TLS record layer + ClientHello SNI | Reading structured bytes off the wire by hand, bounds-checking attacker-controlled lengths |
| Capture format | `libpcap` / pcapng writing | Output that opens in Wireshark, which is how network bugs are actually read |
| Traffic shaping | Token-bucket rate limiting, `SO_LINGER` RST injection, seeded jitter | What `tc netem` and Toxiproxy do, built from the primitives |
| Build | CMake ≥ 3.20 + Ninja | The de-facto C++ build stack; one interface target carries all warning policy |
| Test | CTest driving real processes over real sockets | Integration-testing network code without mocking the thing under test |
| Correctness | `-Werror -Wconversion -Wshadow`, ASan, UBSan, clang-tidy | Where C++ memory safety is won or lost |
| Portability | gcc + clang on Ubuntu and clang on macOS in CI, developed on macOS | `SO_NOSIGPIPE` vs `MSG_NOSIGNAL`, `kqueue` vs `epoll` — the BSD/Linux split is real |
| Inspection | `tcpdump`, Wireshark, `nc`, `curl -v`, `iperf3` | Generating and reading traffic to check the tool against reality |

### Tools worth having alongside it

```bash
brew install cmake ninja clang-format shellcheck wireshark iperf3
```

`nc` and `curl` ship with macOS. Point the tap at a real server
(`--upstream example.com:80`) and run `curl` through it — watching a real HTTP
exchange scroll past in chunk-sized pieces is the fastest way to understand
what a proxy actually sees.

## Testing philosophy

The suite runs the real binaries against each other over real sockets: three
processes, two TCP connections, no mocks. The only way to find out whether a
proxy relays bytes is to relay bytes.

Each test owns its own pair of ports, so `ctest -j` is safe. That is a
constraint worth keeping — a hardcoded shared port means tests that cannot run
concurrently.

See [CONVENTIONS.md](CONVENTIONS.md) for the rules the code follows.
