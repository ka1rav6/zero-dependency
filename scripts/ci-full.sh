#!/usr/bin/env bash
# scripts/ci-full.sh
#
# The last-mile check, run inside the dev-full image (design doc §10.5):
#
#     docker compose --profile full run --rm dev-full ./scripts/ci-full.sh
#
# scripts/ci.sh proves kap builds, its tests pass, and every plugin produces the
# argv arrays its golden files say it should — all without a single ecosystem
# toolchain installed. That is deliberate (§5.2), and it is what makes the test
# suite fast and portable.
#
# What it cannot prove is that those argv arrays are ones the real tools accept.
# A plugin can emit `cargo buidl --release`, match its own golden file
# perfectly, and be completely broken. This script closes that gap by creating a
# real project of each kind and running `kap build` and `kap test` against it
# for real.
#
# It needs Rust, Node, Go, and uv, which is why it lives in its own image and
# its own script rather than in ci.sh.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

echo "== the ordinary CI first ==========================================="
./scripts/ci.sh

kap="$repo_root/build/kap"
export KAP_PLUGIN_PATH="$repo_root/plugins"

# Every scratch project lives here and goes away afterwards, whatever happens.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

passed=0
failed=0
skipped=0

# check <name> <required-tool> <project-dir> <kap args...>
#
# A missing toolchain is a SKIP, not a failure: this script has to stay usable
# outside the dev-full image, where someone may have three of the four.
check() {
    local name="$1" tool="$2" dir="$3"; shift 3
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf '[SKIP] %s (%s is not installed)\n' "$name" "$tool"
        skipped=$((skipped + 1))
        return
    fi
    if ( cd "$dir" && "$kap" "$@" ) >/dev/null 2>&1; then
        printf '[PASS] %s\n' "$name"
        passed=$((passed + 1))
    else
        printf '[FAIL] %s\n' "$name"
        printf '       reproduce with: cd %s && %s %s\n' "$dir" "$kap" "$*"
        ( cd "$dir" && "$kap" "$@" ) 2>&1 | sed 's/^/       /' | head -25 || true
        failed=$((failed + 1))
    fi
}

echo
echo "== cmake-cpp ======================================================="
mkdir -p "$work/cmake"
cat > "$work/cmake/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(kap_full_demo CXX)
add_executable(demo main.cpp)
enable_testing()
add_test(NAME smoke COMMAND demo)
CMAKE
printf '#include <cstdio>\nint main() { std::puts("ok"); return 0; }\n' > "$work/cmake/main.cpp"
check "cmake-cpp build" cmake "$work/cmake" build
check "cmake-cpp test"  cmake "$work/cmake" test
check "cmake-cpp clean" cmake "$work/cmake" clean

echo
echo "== cargo-rust ======================================================"
if command -v cargo >/dev/null 2>&1; then
    ( cd "$work" && cargo new --quiet --bin rust >/dev/null 2>&1 ) || true
fi
check "cargo-rust build" cargo "$work/rust" build
check "cargo-rust test"  cargo "$work/rust" test
check "cargo-rust fmt"   cargo "$work/rust" fmt

echo
echo "== go =============================================================="
mkdir -p "$work/go"
printf 'module example.com/demo\n\ngo 1.22\n' > "$work/go/go.mod"
printf 'package main\n\nimport "fmt"\n\nfunc main() { fmt.Println("ok") }\n' > "$work/go/main.go"
check "go build" go "$work/go" build
check "go check" go "$work/go" check
check "go fmt"   go "$work/go" fmt

echo
echo "== node ============================================================"
mkdir -p "$work/node"
cat > "$work/node/package.json" <<'JSON'
{
  "name": "kap-full-demo",
  "version": "1.0.0",
  "private": true,
  "scripts": {
    "build": "node -e \"console.log('built')\"",
    "test": "node -e \"console.log('tested')\""
  }
}
JSON
check "node build" npm "$work/node" build
check "node test"  npm "$work/node" test

echo
echo "== python-uv ======================================================="
mkdir -p "$work/python/src/demo"
cat > "$work/python/pyproject.toml" <<'TOML'
[project]
name = "demo"
version = "0.1.0"
requires-python = ">=3.11"

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

# ruff has to be a real dependency of the project, not merely installed on the
# machine: every python-uv command is `uv run <tool>`, which resolves inside the
# project's own environment. That is the whole point of the plugin, and a check
# that leaned on a globally installed ruff would not be testing it.
[dependency-groups]
dev = ["ruff"]
TOML
printf 'def main() -> None:\n    print("ok")\n' > "$work/python/src/demo/__init__.py"
check "python-uv install" uv "$work/python" install
check "python-uv lint"    uv "$work/python" lint

echo
echo "== make-generic ===================================================="
mkdir -p "$work/make"
printf 'all:\n\t@echo built\n\ntest:\n\t@echo tested\n\nclean:\n\t@echo cleaned\n' > "$work/make/Makefile"
check "make-generic build" make "$work/make" build
check "make-generic test"  make "$work/make" test

echo
echo "== doctor and ports ================================================"
check "doctor in a real project" cmake "$work/cmake" doctor
check "ports"                    ss    "$work/cmake" ports

echo
printf '\n%d ecosystem checks: %d passed, %d failed, %d skipped\n' \
    "$((passed + failed + skipped))" "$passed" "$failed" "$skipped"
[ "$failed" -eq 0 ] || exit 1
echo "ci-full.sh: all green"
