# CMakeLists.txt
#
# Build system for the kap binary (zero-dependency rule: AGENTS.md §2 and
# design doc §9). The binary links ONLY against the C++ standard library and
# POSIX, so this file deliberately adds no external packages: no FetchContent,
# no third-party find_package. CMake and Ninja are *dev* tools, never linked
# dependencies.

cmake_minimum_required(VERSION 3.16)
project(kap VERSION 0.1.0 DESCRIPTION
        "Zero-config CLI that detects your project and runs the right tool"
        LANGUAGES CXX)

# The design doc pins C++20; require the real standard (no GNU extensions so
# the code stays portable across GCC and Clang).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Compiler hygiene --------------------------------------------------------
# Cheap, portable warnings. Students and newcomers should get squigglies for
# common mistakes before they ship, and CI fails on warnings anyway.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- kap_core library ---------------------------------------------------------
# Shared infrastructure (design doc Milestone 1): TOML parsing + CLI parsing
# live here as real TUs so the binary and the tests link the same code; the
# small header-only modules (diag, argv, fs) are included directly.
add_library(kap_core STATIC
    core/toml.cpp
    core/cli.cpp
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
    tests/test_version.cpp
)
target_link_libraries(kap_tests PRIVATE kap_core)

add_test(NAME kap_unit_tests COMMAND kap_tests)

# End-to-end smoke test: the built binary must print the version banner. The
# regex is built from PROJECT_VERSION so it can never drift from the code.
add_test(NAME kap_version_smoke COMMAND kap --version)
set_tests_properties(
    kap_version_smoke
    PROPERTIES PASS_REGULAR_EXPRESSION "kap ${PROJECT_VERSION}"
)

# Milestone-1 exit-criteria smoke test: `kap config get` reads a TOML file.
# The command runs under CMake's working directory, so we point it at the
# fixture via --root.
add_test(NAME kap_config_get_smoke
         COMMAND kap config get --root "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/config" server.host)
set_tests_properties(
    kap_config_get_smoke
    PROPERTIES PASS_REGULAR_EXPRESSION "localhost"
)