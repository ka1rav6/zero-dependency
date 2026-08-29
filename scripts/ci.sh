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

echo "== install ========================================================"
# Design doc §6.5 tier three: an installed kap must find its own bundled plugins
# and registry from its own location, with no environment variables and no
# configuration. Verified by installing into a scratch prefix and running the
# result with an empty environment — which is also the closest thing to what a
# user gets from scripts/install.sh.
install_prefix="$(mktemp -d)"
cmake --install build --prefix "$install_prefix" >/dev/null
env -i "$install_prefix/bin/kap" --version >/dev/null
bundled="$(env -i "$install_prefix/bin/kap" plugin list | wc -l | tr -d " ")"
if [ "$bundled" -lt 8 ]; then
    echo "ci.sh: an installed kap found only $bundled bundled plugins" >&2
    exit 1
fi
env -i "$install_prefix/bin/kap" plugin search cmake >/dev/null
echo "ci.sh: installed kap sees $bundled bundled plugins with an empty environment"
rm -rf "$install_prefix"

echo "== completions ===================================================="
# A completion script with a syntax error is worse than none: it makes every
# Tab in that shell print an error.
./build/kap completions bash > /tmp/kap-ci-completion.bash
bash -n /tmp/kap-ci-completion.bash
rm -f /tmp/kap-ci-completion.bash
./build/kap completions zsh >/dev/null
./build/kap completions fish >/dev/null

echo "== install script ================================================="
# Only a syntax check here: actually running it clones and rebuilds, which the
# rest of this script has already done. `sh -n` still catches the class of
# mistake that matters most for a script people pipe into a shell.
sh -n scripts/install.sh
bash -n scripts/ci-full.sh

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