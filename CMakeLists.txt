# CMakeLists.txt
#
# Build system for the kap binary (zero-dependency rule: AGENTS.md §2 and
# design doc §9). The binary links ONLY against the C++ standard library and
# POSIX, so this file deliberately adds no external packages: no FetchContent,
# no third-party find_package. CMake and Ninja are *dev* tools, never linked
# dependencies.

cmake_minimum_required(VERSION 3.16)

# --- Version: one source of truth --------------------------------------------
# core/version.hpp is authoritative; the binary reports what is written there.
# Rather than repeat the number here (where it would drift the first time
# someone bumps only one of the two), parse it out of the header. The version
# smoke test then asserts the binary's banner against CMake's PROJECT_VERSION,
# which closes the loop: header, build system, and test can only ever agree.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/core/version.hpp" _kap_version_header)
string(REGEX MATCH "kVersionMajor[ \t]*=[ \t]*([0-9]+)" _m "${_kap_version_header}")
set(KAP_VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "kVersionMinor[ \t]*=[ \t]*([0-9]+)" _m "${_kap_version_header}")
set(KAP_VERSION_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "kVersionPatch[ \t]*=[ \t]*([0-9]+)" _m "${_kap_version_header}")
set(KAP_VERSION_PATCH "${CMAKE_MATCH_1}")

if(KAP_VERSION_MAJOR STREQUAL "" OR KAP_VERSION_MINOR STREQUAL "" OR
   KAP_VERSION_PATCH STREQUAL "")
    message(FATAL_ERROR
            "Could not read the version from core/version.hpp. The kVersion* "
            "constants must stay in the form 'kVersionMajor = 0;'.")
endif()

project(kap VERSION "${KAP_VERSION_MAJOR}.${KAP_VERSION_MINOR}.${KAP_VERSION_PATCH}"
        DESCRIPTION
        "Zero-config CLI that detects your project and runs the right tool"
        LANGUAGES CXX)

# Re-run CMake when the header changes, so a version bump takes effect without
# a manual reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/core/version.hpp")

# The design doc pins C++20; require the real standard (no GNU extensions so
# the code stays portable across GCC and Clang).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Compiler hygiene --------------------------------------------------------
# Cheap, portable warnings. Students and newcomers should get squigglies for
# common mistakes before they ship.
#
# KAP_WERROR promotes them to errors. It is OFF by default so a local build is
# never blocked by a warning from a compiler version we have not seen, and ON
# in CI (scripts/ci.sh passes -DKAP_WERROR=ON) so warnings cannot accumulate on
# main. This is not theoretical: -Wdangling-reference caught a genuine
# use-after-free in the test harness, and -Wmissing-field-initializers flagged
# six under-initialised aggregates in the TOML parser.
option(KAP_WERROR "Treat compiler warnings as errors (used by CI)" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(KAP_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

# --- Sanitizers ---------------------------------------------------------------
# KAP_SANITIZE=ON builds with AddressSanitizer + UndefinedBehaviorSanitizer.
#
# Worth having as a one-flag option rather than something each contributor
# reassembles from memory: kap is mostly parsers walking buffers by index, which
# is exactly the code where an off-by-one reads out of bounds and still passes
# every test. ASan turns those into loud failures.
#
# Off by default (it costs roughly 2x runtime) and never combined with a release
# build. Use it when touching a parser:
#
#     cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKAP_SANITIZE=ON
#     cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
option(KAP_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(KAP_SANITIZE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # The flags must reach the linker too, or the sanitizer runtime is
        # never linked in and the instrumentation silently does nothing.
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "KAP_SANITIZE is only supported for GCC and Clang; ignoring.")
    endif()
endif()

# --- kap_core library ---------------------------------------------------------
# Shared infrastructure (design doc Milestone 1): TOML parsing + CLI parsing
# live here as real TUs so the binary and the tests link the same code; the
# small header-only modules (diag, argv, fs) are included directly.
add_library(kap_core STATIC
    core/toml.cpp
    core/cli.cpp
    core/kpl.cpp
)
# Let `#include "core/..."` resolve from the repository root for consumers.
target_include_directories(kap_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# --- kap binary ---------------------------------------------------------------
add_executable(kap core/main.cpp)
target_link_libraries(kap PRIVATE kap_core)

# --- Tests ---------------------------------------------------------------------
# In-tree, zero-dependency test harness design doc §9 mandates (no Catch2 /
# GTest). The harness header (tests/harness.hpp) is compiled straight into the
# test binary; CTest drives the executables below.
enable_testing()

add_executable(
    kap_tests
    tests/test_main.cpp
    tests/test_harness.cpp
    tests/test_diag.cpp
    tests/test_argv.cpp
    tests/test_fs.cpp
    tests/test_toml.cpp
    tests/test_cli.cpp
    tests/test_kpl.cpp
    tests/test_version.cpp
)
target_link_libraries(kap_tests PRIVATE kap_core)

add_test(NAME kap_unit_tests COMMAND kap_tests)

# End-to-end tests: everything the unit tests structurally cannot reach —
# exit codes, stdout/stderr separation, and argument dispatch through the real
# binary. tests/e2e.sh is a POSIX-shell driver (no bats/shunit2, per AGENTS.md
# §6) that takes the binary's path as its only argument.
add_test(
    NAME kap_e2e
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh" "$<TARGET_FILE:kap>" "${PROJECT_VERSION}"
)

# Milestone-0 contract, kept as its own test so a broken version banner is
# reported independently of the wider e2e run. The regex is built from
# PROJECT_VERSION, which CMake reads out of core/version.hpp above, so the
# assertion cannot drift from the code.
add_test(NAME kap_version_smoke COMMAND kap --version)
set_tests_properties(
    kap_version_smoke
    PROPERTIES PASS_REGULAR_EXPRESSION "kap ${PROJECT_VERSION}"
)