#!/usr/bin/env bash
# scripts/ci.sh
#
# One-shot CI entry point (design doc §10.6), always run inside the dev
# container so host toolchains can never diverge:
#
#     docker compose run --rm dev ./scripts/ci.sh
#
# Configures, builds, runs the unit tests, enforces formatting, and
# smoke-tests the binary. Later milestones add their own steps here:
#   - Milestone 8: `kap plugin test` for every first-party plugin
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

echo "== configure ======================================================"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKAP_WERROR=ON

echo "== build =========================================================="
cmake --build build

echo "== unit tests ====================================================="
ctest --test-dir build --output-on-failure

echo "== formatting ====================================================="
clang-format --dry-run --Werror core/*.cpp core/*.hpp tests/*.cpp tests/*.hpp

echo "== plugin checks =================================================="
# Design doc §10.6: ci.sh runs `kap plugin test` for every first-party plugin.
# `doctor` parses, manifest-validates, and type-checks each one; `test`
# evaluates its fixture cases against committed golden CommandSpecs. Neither
# executes a real build tool, so this stage needs no ecosystem toolchain.
KAP_PLUGIN_PATH="$repo_root/plugins" ./build/kap plugin doctor --root .
KAP_PLUGIN_PATH="$repo_root/plugins" ./build/kap plugin test --root .

echo "== registry ======================================================="
# The registry index is a real file this project ships (design doc §6.2). A
# typo in it should fail here rather than the first time somebody installs.
./build/kap plugin search cmake --root . >/dev/null
./build/kap plugin list --root . >/dev/null

echo "== detection ======================================================"
# Dogfooding: kap's own repository is a CMake project, so the bundled cmake-cpp
# plugin must claim it. This is the cheapest possible check that the detection
# engine, plugin discovery, and the KPL loader agree with each other on a real
# tree rather than only on synthetic fixtures.
detected="$(KAP_PLUGIN_PATH="$repo_root/plugins" ./build/kap detect --refresh --root . | head -1)"
case "$detected" in
    cmake-cpp*) echo "ci.sh: detect resolved '$detected'" ;;
    *) echo "ci.sh: expected detect to resolve cmake-cpp, got '$detected'" >&2; exit 1 ;;
esac

echo "== smoke tests ===================================================="
# Milestone-0 contract: version banner, help, and a loud failure for any
# command we do not know yet.
./build/kap --version
./build/kap --help >/dev/null
if ./build/kap --definitely-not-a-real-command >/dev/null 2>&1; then
    echo "ci.sh: an unknown command should have failed but did not" >&2
    exit 1
fi

echo "ci.sh: all green"