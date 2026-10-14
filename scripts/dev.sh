#!/usr/bin/env bash
# tcp-tap dev CLI — one entry point for every routine task.
#
#   ./scripts/dev.sh build            configure + build (Debug)
#   ./scripts/dev.sh build --release  optimized build
#   ./scripts/dev.sh build --asan     build with AddressSanitizer + UBSan
#   ./scripts/dev.sh build --werror   warnings become errors (what CI does)
#   ./scripts/dev.sh tap [args...]    run the proxy (default: 9100 -> 9101)
#   ./scripts/dev.sh echo [port]      run the echo upstream (default 9101)
#   ./scripts/dev.sh send [port] [msg] run the client (default 9100)
#   ./scripts/dev.sh demo             echo + tap + send, end to end, in one go
#   ./scripts/dev.sh test             build, then run the CTest suite
#   ./scripts/dev.sh lint             clang-tidy over every source file
#   ./scripts/dev.sh fmt              clang-format in place
#   ./scripts/dev.sh fmt --check      fail if anything is unformatted (CI mode)
#   ./scripts/dev.sh ci               everything CI runs, locally
#   ./scripts/dev.sh branch <name>    start a feature/<name> branch off main
#   ./scripts/dev.sh pr               push the branch and open a PR against main
#   ./scripts/dev.sh clean            delete build directories
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

build_dir="build"
sources=(src/proxy/tap.cpp src/proxy/event_loop.h src/testing/echo_server.cpp src/testing/send_client.cpp)
shell_scripts=(
  scripts/dev.sh
  scripts/install_hooks.sh
  tests/smoke_test.sh
  tests/refused_test.sh
  .githooks/commit-msg
)

die() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

generator() {
  if have ninja; then echo "Ninja"; else echo "Unix Makefiles"; fi
}

cmd_build() {
  local build_type="Debug" asan="OFF" werror="OFF" arg
  for arg in "$@"; do
    case "${arg}" in
      --release) build_type="RelWithDebInfo" ;;
      --asan)    asan="ON"; build_dir="build-asan" ;;
      --werror)  werror="ON" ;;
      *) die "unknown build flag: ${arg}" ;;
    esac
  done

  have cmake || die "cmake not found (brew install cmake)"

  cmake -S . -B "${build_dir}" -G "$(generator)" \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DENABLE_ASAN="${asan}" \
    -DTCPTAP_WERROR="${werror}" \
    -DBUILD_TESTING=ON
  cmake --build "${build_dir}"
  echo
  echo "binaries in ${build_dir}/: tcptap, tcptap-echo, tcptap-send"
}

# Build only if the binaries are not already sitting in the directory the
# requested flags imply.
ensure_built() {
  local arg
  for arg in "$@"; do
    [ "${arg}" = "--asan" ] && build_dir="build-asan"
  done
  [ -x "${build_dir}/tcptap" ] || cmd_build "$@"
}

cmd_tap() {
  ensure_built
  if [ "$#" -eq 0 ]; then
    set -- --listen 9100 --upstream 127.0.0.1:9101
  fi
  exec "${build_dir}/tcptap" "$@"
}

cmd_echo() {
  ensure_built
  exec "${build_dir}/tcptap-echo" "${1:-9101}"
}

cmd_send() {
  ensure_built
  exec "${build_dir}/tcptap-send" "${1:-9100}" "${2:-hello-through-the-tap}"
}

# The three-process chain a human would otherwise set up across three terminals.
#
# The pids are script-scope, not local: the EXIT trap runs after this function
# has already returned, and a `local` would be out of scope by then — which,
# under `set -u`, aborts the cleanup instead of performing it.
demo_echo_pid=""
demo_tap_pid=""

demo_cleanup() {
  [ -n "${demo_tap_pid}" ] && kill "${demo_tap_pid}" 2>/dev/null
  [ -n "${demo_echo_pid}" ] && kill "${demo_echo_pid}" 2>/dev/null
  return 0
}

cmd_demo() {
  ensure_built
  trap demo_cleanup EXIT

  echo "--- upstream ---"
  "${build_dir}/tcptap-echo" 9101 &
  echo_pid=$!
  sleep 0.3

  echo "--- proxy ---"
  "${build_dir}/tcptap" --listen 9100 --upstream 127.0.0.1:9101 --once &
  tap_pid=$!
  sleep 0.3

  echo "--- client ---"
  "${build_dir}/tcptap-send" 9100 "hello-through-the-tap"

  wait "${tap_pid}" 2>/dev/null || true
  tap_pid=""
  wait "${echo_pid}" 2>/dev/null || true
  echo_pid=""
}

cmd_test() {
  ensure_built "$@"
  ctest --test-dir "${build_dir}" --output-on-failure
}

cmd_lint() {
  have clang-tidy || die "clang-tidy not found (brew install llvm, then add it to PATH)"
  [ -f "${build_dir}/compile_commands.json" ] || cmd_build
  clang-tidy -p "${build_dir}" "${sources[@]}"
}

cmd_fmt() {
  have clang-format || die "clang-format not found (brew install clang-format)"
  if [ "${1:-}" = "--check" ]; then
    clang-format --dry-run --Werror "${sources[@]}"
    echo "format: clean"
  else
    clang-format -i "${sources[@]}"
    echo "format: applied"
  fi
}

# Everything the GitHub workflow runs. With --strict a missing tool is a failure
# rather than a skip: the unattended committer gates on this, and a gate that
# quietly checks less than CI does is not a gate.
cmd_ci() {
  local strict="no"
  [ "${1:-}" = "--strict" ] && strict="yes"

  require() {
    have "$1" && return 0
    [ "${strict}" = "yes" ] && die "$1 not found and --strict was requested ($2)"
    echo "skip: $1 not installed ($2)"
    return 1
  }

  cmd_build --werror
  ctest --test-dir "${build_dir}" --output-on-failure
  if require clang-format "brew install clang-format"; then cmd_fmt --check; fi
  if require shellcheck "brew install shellcheck"; then shellcheck "${shell_scripts[@]}"; fi
  echo
  if [ "${strict}" = "yes" ]; then echo "ci: all green (strict)"; else echo "ci: all green"; fi
}

cmd_branch() {
  local name="${1:-}"
  [ -n "${name}" ] || die "usage: dev.sh branch <name>"
  git checkout main
  git pull --ff-only
  git checkout -b "feature/${name}"
}

cmd_pr() {
  have gh || die "gh not found (brew install gh)"
  local branch
  branch="$(git rev-parse --abbrev-ref HEAD)"
  [ "${branch}" != "main" ] || die "refusing to open a PR from main"
  git push -u origin "${branch}"
  gh pr create --base main --fill
}

cmd_clean() {
  rm -rf build build-asan
  echo "removed build/ and build-asan/"
}

cmd_help() { sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

case "${1:-help}" in
  build)  shift; cmd_build "$@" ;;
  tap)    shift; cmd_tap "$@" ;;
  echo)   shift; cmd_echo "$@" ;;
  send)   shift; cmd_send "$@" ;;
  demo)   shift; cmd_demo "$@" ;;
  test)   shift; cmd_test "$@" ;;
  lint)   shift; cmd_lint "$@" ;;
  fmt)    shift; cmd_fmt "$@" ;;
  ci)     shift; cmd_ci "$@" ;;
  branch) shift; cmd_branch "$@" ;;
  pr)     shift; cmd_pr "$@" ;;
  clean)  shift; cmd_clean "$@" ;;
  help|-h|--help) cmd_help ;;
  *) die "unknown command: $1 (try: dev.sh help)" ;;
esac
