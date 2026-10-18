# Conventions

The rules this code follows. Most of them exist because a socket program that
ignores them appears to work on a loopback connection and fails on a real one.

## Syscalls

- **Check every return value.** `socket`, `setsockopt`, `bind`, `listen`,
  `accept`, `connect`, `recv`, `send`, `shutdown`, `close`, `inet_pton`,
  `getaddrinfo`, `poll` — all of them. On failure print to `stderr` with
  `std::strerror(errno)` and return non-zero.
- `getaddrinfo` reports errors through `gai_strerror`, **not** `errno`. Using
  `errno` there prints an unrelated message, which is worse than printing none.
- **Close every descriptor on every path, including error paths.** A `return 1`
  between `socket()` and `close()` is a leak. `dial()` closes each candidate
  address it rejects — keep that pattern.

## recv and send

- `recv` has three outcomes and all three must be handled: `-1` is an error
  (retry on `EINTR`, bail otherwise), `0` means the peer closed its half, `> 0`
  is a byte count. Once sockets go non-blocking, `EAGAIN` / `EWOULDBLOCK`
  becomes a fourth case, and it is **not** an error.
- **A `recv` buffer is not a string.** It is not NUL-terminated and may contain
  any byte, including zero. Print it through `std::string_view(buf, n)`, never
  `std::cout << buf`.
- Always bound the read by `sizeof(buf)`.
- **`send` may transmit fewer bytes than asked.** In the proxy the unsent rest
  stays in that direction's pending buffer until the far side has room; the
  test programs use a `send_all` loop. Anything else needs a comment explaining
  why a partial send is impossible at that call site.

## Half-close

A proxy forwards an EOF; it does not treat one as teardown. When one side stops
writing, `shutdown(other, SHUT_WR)` and keep relaying the other direction until
it closes too. `ENOTCONN` from that `shutdown` is benign — it means the peer got
there first.

## Addresses and byte order

- `htons` / `htonl` going out, `ntohs` / `ntohl` coming in. A missing conversion
  is a bug even when it happens to work on this machine.
- `reinterpret_cast<sockaddr*>(&addr)` is the expected cast for
  `bind` / `connect` / `accept`. It is not a smell here.
- `inet_ntop` to print, `inet_pton` to parse. Never `inet_addr` — it cannot
  report failure distinguishably from the valid address `255.255.255.255`.
- `sizeof(...)` passed as a `socklen_t` needs an explicit
  `static_cast<socklen_t>`. `-Wconversion` is on and brace init will not let the
  narrowing through silently.

## C++ style

- C++20. Brace initialization (`int fd{socket(...)}`) — it is the style in the
  tree and it catches narrowing.
- Errors to `stderr`, normal output to `stdout`.
- **`std::cout` is block-buffered when redirected.** Anything a long-running
  process prints may sit in the buffer indefinitely. The tests grep the
  `listening` lines of live processes, so **those lines must stay `std::endl`**.
  Read the header comment in `tests/smoke_test.sh` before changing how any of
  this logs.
- Readable over clever. A comment explaining *why* a syscall is called the way
  it is earns its place; one restating what the next line obviously does does
  not.
- Formatting is `clang-format`'s problem, not yours: `./scripts/dev.sh fmt`.

## Security

- Never commit secrets, API keys, or credentials.
- **Never trust a length that came off the wire without bounding it against the
  buffer.** This matters most in the TLS and HTTP parsing: a ClientHello is
  attacker-controlled input, and its internal length fields are the classic
  place to walk off the end of an allocation.
- Bind to loopback by default. `--listen-host` is the only way to bind wider,
  and it warns when the address is not loopback.

## Portability

Developed on macOS, built on Linux in CI, and the socket APIs differ.
`SO_NOSIGPIPE` is BSD/macOS only; Linux suppresses `SIGPIPE` with `MSG_NOSIGNAL`
on each `send` instead. The `kSendFlags` / `suppress_sigpipe` pair in every
source file shows the pattern — guard a platform option with `#ifdef` and
provide the other platform's equivalent. A macOS-only symbol fails the build.

The same caution applies to the test scripts: macOS ships bash 3.2, which has
no negative array indices. `tests/lib.sh`'s `start` helper leaves the pid of
the process it just backgrounded in `last_pid`, so a test reads that instead
of reaching for `"${pids[-1]}"`.

The event-loop backends follow a stricter version of the same rule. Each one
lives in `src/proxy/event_loop.h` under one branch of a single
`#if`/`#elif` chain keyed on the platform macro (`__APPLE__` and the BSDs for
`kqueue`, `__linux__` for `epoll`), with `poll()` as the final `#else`. Every
branch defines the same `EventLoop` with the same three calls and the same
meaning, so the relay never needs an `#ifdef` of its own. A new backend adds a
branch to that chain, and a CI job on a platform that selects it.

## Build system

CMake ≥ 3.20, C++20, Ninja when available. Three binaries and one interface
target (`tcptap_flags`) carrying the warning and sanitizer policy — new targets
link it so flags cannot drift.

- `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` everywhere.
- `-DTCPTAP_WERROR=ON` turns those into errors. CI builds with it on.
- `-DENABLE_ASAN=ON` adds AddressSanitizer + UBSan, into `build-asan/`.

Use `./scripts/dev.sh` rather than raw `cmake` / `ctest`, so local runs match CI.

## Tests

New behaviour needs a test unless there is a reason it cannot have one — say so
in the commit when that happens. A new program goes in `src/` and gets its own
`add_executable` linking `tcptap_flags`.

Each test owns its own pair of ports so `ctest -j` is safe. Keep it that way.

## Commits

- Imperative subject, 72 characters or fewer: "handle partial sends in the
  relay", not "handled partial sends" and not "updates".
- The body explains *why* when the diff does not already say it. Wrap at 72.
- One logical change per commit.
- **No attribution trailers.** No `Co-Authored-By`, no "Generated with", no tool
  banners, no emoji. The log is a record of changes to this program, and
  anything else is noise. `.githooks/commit-msg` enforces this; run
  `./scripts/install_hooks.sh` once per clone to activate it.

## CI

`.github/workflows/ci.yml` runs on every push and PR:

- `build (gcc)` / `build (clang)` — configure, build with `-Werror`, run CTest
  on Linux, which selects the `epoll` backend
- `build (gcc, poll() fallback)` — the same with `-DTCPTAP_FORCE_POLL=ON`
- `build (macOS clang)` — the same on macOS, which selects `kqueue`
- `tests (ASan + UBSan)` — the same suite under sanitizers
- `clang-format` — formatting check
- `shellcheck` — `scripts/*.sh`, `tests/*.sh`, and the hook

Every one of those is also run by `./scripts/dev.sh ci --strict`, deliberately,
with one exception: a machine builds only the backend its own platform selects
(plus `poll()` with `-DTCPTAP_FORCE_POLL=ON`). The other platform's backend is
checked in CI, so a change to it should be built there before it is relied on.
