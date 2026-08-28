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
#   - Milestone 2+: `kap plugin doctor` golden-parses every .kpl fixture
#   - Milestone 8:  `kap plugin test` for every first-party plugin
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

echo "== configure ======================================================"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

echo "== build =========================================================="
cmake --build build

echo "== unit tests ====================================================="
ctest --test-dir build --output-on-failure

echo "== formatting ====================================================="
clang-format --dry-run --Werror core/*.cpp core/*.hpp tests/*.cpp tests/*.hpp

echo "== smoke tests ===================================================="
# Milestone-0 contract: version banner, help, and a loud failure for any
# command we do not know yet.
./build/kap --version
./build/kap --help >/dev/null
if ./build/kap --definitely-not-a-real-command >/dev/null 2>&1; then
    echo "ci.sh: an unknown command should have failed but did not" >&2
    exit 1
fi

# TODO(Milestone 2+): golden-parse every .kpl fixture via `kap plugin doctor`.
# TODO(Milestone 8):  run `kap plugin test` for every first-party plugin.

echo "ci.sh: all green"