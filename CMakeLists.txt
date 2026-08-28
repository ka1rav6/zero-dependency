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

# --- kap binary ---------------------------------------------------------------
# Milestone 0: an empty binary that prints its version and exits 0. The real
# subsystems (cli, detection, kpl) arrive in later milestones.
add_executable(kap core/main.cpp)

# Let `#include "core/version.hpp"` resolve from the repository root.
target_include_directories(kap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# --- Tests ---------------------------------------------------------------------
# In-tree, zero-dependency test harness design doc §9 mandates (no Catch2 /
# GTest). The harness header (tests/harness.hpp) is compiled straight into the
# test binary; CTest drives the executables below.
enable_testing()

add_executable(
    kap_tests
    tests/test_main.cpp
    tests/test_version.cpp
)
target_include_directories(kap_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME kap_unit_tests COMMAND kap_tests)

# End-to-end smoke test: the built binary must print the version banner. The
# regex is built from PROJECT_VERSION so it can never drift from the code.
add_test(NAME kap_version_smoke COMMAND kap --version)
set_tests_properties(
    kap_version_smoke
    PROPERTIES PASS_REGULAR_EXPRESSION "kap ${PROJECT_VERSION}"
)