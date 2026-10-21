# tcp-tap

A fault-injecting TCP proxy built straight on the POSIX sockets API. It sits
between a client and a server, relays bytes in both directions, logs every
chunk it carries — and is being taught to degrade the link on purpose, so the
programs on either end can be tested against a network that misbehaves.

No frameworks and no dependencies. Every networking library is a wrapper around
the same dozen system calls; here the syscalls are the point.

Most of this code is written by an AI coding agent from a plan I maintain.

```
$ ./scripts/dev.sh demo
--- upstream ---
echo listening on 127.0.0.1:9101
--- proxy ---
listening on 127.0.0.1:9100, upstream 127.0.0.1:9101
--- client ---
sent 21 bytes [hello-through-the-tap]
accepted 127.0.0.1:57137
c>u 21 bytes [hello-through-the-tap]
c>u eof
echo 21 bytes hello-through-the-tap
echo saw eof
u>c 21 bytes [hello-through-the-tap]
u>c eof
closed: 21 bytes upstream, 21 bytes downstream
got 21 bytes [hello-through-the-tap]
```

`c>u` is downstream-to-upstream, `u>c` the other way.

## Getting started

```bash
brew install cmake ninja clang-format shellcheck   # apt: cmake ninja-build clang-format shellcheck
./scripts/install_hooks.sh                         # once per clone
./scripts/dev.sh build
./scripts/dev.sh demo
```

Or drive the three processes yourself, in three terminals:

```bash
./scripts/dev.sh echo 9101                          # the upstream
./scripts/dev.sh tap --listen 9100 --upstream 127.0.0.1:9101
./scripts/dev.sh send 9100 "hello-through-the-tap"  # the client
```

Anything that speaks TCP works as the upstream — point it at a real server and
watch the conversation:

```bash
./scripts/dev.sh tap --listen 8080 --upstream example.com:80
curl -v http://127.0.0.1:8080/
```

## Tech stack

| Layer | Choice |
| --- | --- |
| Language | C++20, no dependencies |
| Network API | POSIX / BSD sockets — `socket`, `bind`, `listen`, `accept`, `connect`, `recv`, `send`, `shutdown` |
| Multiplexing | `select()` today; `poll()`, then `kqueue` (BSD/macOS) and `epoll` (Linux) |
| Protocols read by hand | TCP half-close & RST, HTTP/1.1 request lines, TLS record layer + ClientHello SNI |
| Traffic shaping | Seeded delay/jitter, rechunking, token-bucket rate limits, bit corruption, mid-stream RST |
| Capture output | `libpcap` / pcapng, readable in Wireshark |
| Build | CMake ≥ 3.20 + Ninja |
| Test | CTest driving the real binaries over real sockets |
| Correctness | `-Werror -Wconversion -Wshadow`, AddressSanitizer, UBSan |
| CI | GitHub Actions — gcc + clang on Ubuntu |

Why each of those, and what it teaches, is in
[docs/OVERVIEW.md](docs/OVERVIEW.md).

## The dev CLI

Everything routine goes through `./scripts/dev.sh`:

| Command | What it does |
| --- | --- |
| `build [--release\|--asan\|--werror]` | Configure + build |
| `tap [args...]` | Run the proxy (default `9100` → `127.0.0.1:9101`) |
| `echo [port]` | Run the echo upstream (default `9101`) |
| `send [port] [msg]` | Run the client (default `9100`) |
| `demo` | Upstream + proxy + client, end to end, in one command |
| `test` | Build, then run the CTest suite |
| `lint` | clang-tidy over every source file |
| `fmt [--check]` | clang-format |
| `ci [--strict]` | Everything CI runs, locally. `--strict` fails on a missing tool |
| `branch <name>` | Start `feature/<name>` off `main` |
| `pr` | Push the branch and open a PR against `main` |
| `clean` | Remove `build/` and `build-asan/` |

## Repo layout

```
src/proxy/
  tap.cpp              the proxy — select() relay, chunk logging, half-close forwarding
src/testing/
  echo_server.cpp      test upstream: accepts one connection, echoes it back
  send_client.cpp      test client: sends, half-closes, reads the reply
CMakeLists.txt         build + test definitions
tests/
  smoke_test.sh        end-to-end relay over real sockets
  refused_test.sh      a dead upstream must be reported, not silently tolerated
scripts/
  dev.sh               the dev CLI
  install_hooks.sh     points core.hooksPath at .githooks/
docs/
  OVERVIEW.md          what it is, why raw syscalls, the three hard problems
  CONVENTIONS.md       the rules the code follows
```

## Status

v1 works: one connection at a time, relayed with `select()`, every chunk logged
with its direction, half-closes forwarded so each direction can drain
independently, and a dead upstream reported rather than silently tolerated.
Three end-to-end tests cover the relay, a longer payload, and a refused
upstream.

Where it goes, roughly in order:

1. **A real event loop** — serve connections back to back, then concurrently:
   `poll()`, non-blocking sockets, per-direction write buffers for backpressure,
   and finally `kqueue` / `epoll`.
2. **The fault injection** — seeded `--delay`, `--jitter`, `--chunk`, `--rate`,
   `--corrupt`, `--drop-after`, `--rst`, composable in a fixed order, with named
   `--profile` presets for the common shapes of bad network.
3. **Observability** — hexdump output, byte accounting, log levels, and pcap
   files that open in Wireshark.
4. **Protocol awareness** — IPv6, HTTP request-line peeking, TLS record and SNI
   parsing, and SNI-based upstream routing.

## Tests

```bash
./scripts/dev.sh test            # CTest suite
./scripts/dev.sh test --asan     # same suite under ASan + UBSan
./scripts/dev.sh ci --strict     # everything CI runs, nothing skipped
```

The tests run the real binaries against each other over real sockets — three
processes, two TCP connections, no mocks. The only way to find out whether a
proxy relays bytes is to relay bytes.

## License

MIT. See [LICENSE](LICENSE).
