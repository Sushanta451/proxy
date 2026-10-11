cmake_minimum_required(VERSION 3.20)
project(tcp-tap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TCPTAP_WERROR "Treat warnings as errors (CI builds with this on)" OFF)
option(ENABLE_ASAN "Build with AddressSanitizer and UBSan" OFF)

# One interface target carries the warning and sanitizer policy. Every binary
# links it, so the flags can never drift between targets.
add_library(tcptap_flags INTERFACE)
target_compile_options(tcptap_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

if(TCPTAP_WERROR)
  target_compile_options(tcptap_flags INTERFACE -Werror)
endif()

if(ENABLE_ASAN)
  target_compile_options(tcptap_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(tcptap_flags INTERFACE -fsanitize=address,undefined)
endif()

add_executable(tcptap src/proxy/tap.cpp)
target_link_libraries(tcptap PRIVATE tcptap_flags)

# Test fixtures. They are real socket programs rather than mocks, because the
# only way to find out whether the proxy relays bytes is to relay bytes.
add_executable(tcptap-echo src/testing/echo_server.cpp)
target_link_libraries(tcptap-echo PRIVATE tcptap_flags)

add_executable(tcptap-send src/testing/send_client.cpp)
target_link_libraries(tcptap-send PRIVATE tcptap_flags)

include(CTest)

if(BUILD_TESTING)
  # Every test owns its own pair of ports, so ctest -j is safe. Keep it that
  # way: a hardcoded shared port means tests that cannot run concurrently.
  add_test(NAME relay-short
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh"
            "$<TARGET_FILE_DIR:tcptap>" 9100 9101 "hello-through-the-tap")

  add_test(NAME relay-longer
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh"
            "$<TARGET_FILE_DIR:tcptap>" 9110 9111
            "the-second-case-relays-a-longer-payload")

  add_test(NAME upstream-refused
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/refused_test.sh"
            "$<TARGET_FILE_DIR:tcptap>" 9120 9121)

  add_test(NAME sequential-connections
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/sequential_test.sh"
            "$<TARGET_FILE_DIR:tcptap>" 9130 9131)

  add_test(NAME concurrent-connections
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrent_test.sh"
            "$<TARGET_FILE_DIR:tcptap>" 9140 9141)

  add_test(NAME large-payload
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/large_payload_test.sh"
            "$<TARGET_FILE_DIR:tcptap>" 9150 9151)

  set_tests_properties(relay-short relay-longer upstream-refused
    sequential-connections concurrent-connections large-payload
    PROPERTIES TIMEOUT 30)
endif()
