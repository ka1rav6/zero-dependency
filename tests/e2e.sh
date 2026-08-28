#!/usr/bin/env bash
# tests/e2e.sh
#
# End-to-end tests for the `kap` binary.
#
# The C++ unit tests in tests/test_*.cpp cover each module in isolation. They
# cannot cover what a *user* experiences: exit codes, which stream a message
# lands on, and whether a mistyped command is refused instead of silently doing
# something else. Those are properties of the assembled binary, so they are
# checked here by running it.
#
# Zero-dependency, same as everything else: POSIX shell and coreutils only, no
# bats/shunit2 (AGENTS.md §6).
#
# Usage:
#     tests/e2e.sh <path-to-kap-binary> <expected-version>
#
# CTest invokes it via the `kap_e2e` test registered in CMakeLists.txt, passing
# CMake's PROJECT_VERSION — which CMake itself reads out of core/version.hpp.
# The expected version is a parameter rather than a literal precisely so this
# file is not a fourth place the version number has to be bumped by hand.

set -uo pipefail   # NOT -e: a failing assertion must be recorded, not fatal

kap_bin="${1:?usage: e2e.sh <path-to-kap-binary> <expected-version>}"
kap_version="${2:?usage: e2e.sh <path-to-kap-binary> <expected-version>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixture="$repo_root/tests/fixtures/config"

passed=0
failed=0

# Report one result. Every assertion funnels through here so the output format
# matches the C++ harness ([PASS]/[FAIL] + a summary line).
pass() { printf '[PASS] %s\n' "$1"; passed=$((passed + 1)); }
fail() { printf '[FAIL] %s\n      %s\n' "$1" "$2"; failed=$((failed + 1)); }

# --- assertion helpers ------------------------------------------------------------

# expect_status <description> <expected-status> <args...>
expect_status() {
    local desc="$1" want="$2"; shift 2
    "$kap_bin" "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then
        pass "$desc"
    else
        fail "$desc" "expected exit status $want, got $got  (kap $*)"
    fi
}

# expect_stdout <description> <expected-exact-stdout> <args...>
expect_stdout() {
    local desc="$1" want="$2"; shift 2
    local got
    got="$("$kap_bin" "$@" 2>/dev/null)"
    if [ "$got" = "$want" ]; then
        pass "$desc"
    else
        fail "$desc" "expected stdout '$want', got '$got'  (kap $*)"
    fi
}

# expect_stderr_contains <description> <needle> <args...>
expect_stderr_contains() {
    local desc="$1" needle="$2"; shift 2
    local got
    got="$("$kap_bin" "$@" 2>&1 >/dev/null)"
    case "$got" in
        *"$needle"*) pass "$desc" ;;
        *)           fail "$desc" "expected stderr to contain '$needle', got '$got'  (kap $*)" ;;
    esac
}

# expect_stdout_contains <description> <needle> <args...>
expect_stdout_contains() {
    local desc="$1" needle="$2"; shift 2
    local got
    got="$("$kap_bin" "$@" 2>/dev/null)"
    case "$got" in
        *"$needle"*) pass "$desc" ;;
        *)           fail "$desc" "expected stdout to contain '$needle', got '$got'  (kap $*)" ;;
    esac
}

# expect_empty_stdout <description> <args...>
expect_empty_stdout() {
    local desc="$1"; shift
    local got
    got="$("$kap_bin" "$@" 2>/dev/null)"
    if [ -z "$got" ]; then
        pass "$desc"
    else
        fail "$desc" "expected empty stdout, got '$got'  (kap $*)"
    fi
}

# --- version and help -------------------------------------------------------------

expect_stdout "--version prints the banner" "kap $kap_version" --version
expect_stdout "-V is the short form of --version" "kap $kap_version" -V
expect_status "--version exits 0" 0 --version
expect_stdout "a bare invocation prints the version" "kap $kap_version"
expect_status "a bare invocation exits 0" 0
expect_status "--help exits 0" 0 --help
expect_stderr_contains "--help goes to stdout, so stderr stays clean" "" --help

# Diagnostics must never pollute stdout: `kap config get x | read` has to be
# safe to pipe.
expect_empty_stdout "an unknown command writes nothing to stdout" definitely-not-a-command
expect_status "an unknown command exits 2" 2 definitely-not-a-command
expect_stderr_contains "an unknown command names the offender" \
    "unknown command 'definitely-not-a-command'" definitely-not-a-command

# --- config get -------------------------------------------------------------------

expect_stdout "config get reads a string from a table" "localhost" \
    config get --root "$fixture" server.host
expect_stdout "config get reads an integer" "8080" \
    config get --root "$fixture" server.port
expect_stdout "config get walks a dotted path with a dashed key" "ninja" \
    config get --root "$fixture" plugins.cmake-cpp.generator
expect_stdout "config get renders an array space-separated" "-DCMAKE_BUILD_TYPE=Debug" \
    config get --root "$fixture" plugins.cmake-cpp.cmake_args
expect_status "config get exits 0 on a hit" 0 config get --root "$fixture" server.host

expect_status "config get exits 1 for a missing key" 1 config get --root "$fixture" no.such.key
expect_stderr_contains "a missing key names the key and the file" "no key 'no.such.key'" \
    config get --root "$fixture" no.such.key
expect_empty_stdout "a missing key prints nothing to stdout" \
    config get --root "$fixture" no.such.key

expect_status "config get exits 1 when there is no config file" 1 \
    config get --root /nonexistent-directory-xyz some.key
expect_stderr_contains "a missing config file says so" "no configuration file at" \
    config get --root /nonexistent-directory-xyz some.key

# --- config subcommand dispatch ---------------------------------------------------
# Regression guard: `run_config` once ran a `get` for every subcommand, so these
# three all "succeeded" while doing the wrong thing.

expect_status "an unknown config subcommand exits 2" 2 \
    config nonsense --root "$fixture" server.host
expect_stderr_contains "an unknown config subcommand lists the valid ones" "expected one of: get, set, edit" \
    config nonsense --root "$fixture" server.host
expect_empty_stdout "an unknown config subcommand prints no value" \
    config nonsense --root "$fixture" server.host

expect_status "config set is refused until Milestone 6" 2 config set a b
expect_stderr_contains "config set explains when it arrives" "not implemented yet" config set a b
expect_status "config edit is refused until Milestone 6" 2 config edit
expect_status "bare 'kap config' shows usage and exits 2" 2 config
expect_status "config get with no key exits 2" 2 config get --root "$fixture"
expect_status "config get with two keys exits 2" 2 config get --root "$fixture" a b

# --- global flag parsing ----------------------------------------------------------

expect_stdout "--root=value form works" "localhost" \
    config get --root="$fixture" server.host
expect_stdout "global flags may follow the command" "localhost" \
    config get server.host --root "$fixture"
expect_stdout "--verbose logs to stderr without disturbing stdout" "localhost" \
    --verbose config get --root "$fixture" server.host
expect_stderr_contains "--verbose reports which file is read" "reading" \
    --verbose config get --root "$fixture" server.host

expect_status "an unknown global flag exits 1" 1 --bogus-flag config get x
expect_stderr_contains "an unknown flag is a located diagnostic" "<argv>" --bogus-flag config get x
expect_status "--set without '=' exits 1" 1 --set nope config get x
expect_status "--root with no value exits 1" 1 config get x --root

# The KPL AST cache (design doc §5.14) must be transparent: the second run
# reads a .kapc instead of re-parsing, and produces exactly the same results.
cache_dir="$(mktemp -d)"
first="$(XDG_CACHE_HOME="$cache_dir" "$kap_bin" plugin test --root "$repo_root" 2>/dev/null)"
second="$(XDG_CACHE_HOME="$cache_dir" "$kap_bin" plugin test --root "$repo_root" 2>/dev/null)"
if [ "$first" = "$second" ] && [ -n "$first" ]; then
    pass "a cached run produces identical results"
else
    fail "a cached run produces identical results" "first='$first' second='$second'"
fi
if ls "$cache_dir"/kap/ast/*.kapc >/dev/null 2>&1; then
    pass "plugin test writes .kapc cache entries"
else
    fail "plugin test writes .kapc cache entries" "no .kapc under $cache_dir/kap/ast"
fi
# A corrupt entry is a cache miss, never a failure.
for entry in "$cache_dir"/kap/ast/*.kapc; do printf 'garbage' > "$entry"; done
if XDG_CACHE_HOME="$cache_dir" "$kap_bin" plugin test --root "$repo_root" >/dev/null 2>&1; then
    pass "a corrupt cache entry falls back to parsing"
else
    fail "a corrupt cache entry falls back to parsing" "plugin test failed after corruption"
fi
rm -rf "$cache_dir"

# --- malformed config -------------------------------------------------------------

bad_dir="$(mktemp -d)"
trap 'rm -rf "$bad_dir"' EXIT

# --- plugin doctor ---------------------------------------------------------------

expect_status "plugin doctor validates bundled plugins" 0 plugin doctor --root "$repo_root"

mkdir -p "$bad_dir/plugins/broken"
printf 'manifest { name = "broken"\n' > "$bad_dir/plugins/broken/plugin.kpl"
expect_status "plugin doctor rejects a malformed plugin" 1 plugin doctor --root "$bad_dir"
expect_stderr_contains "plugin doctor preserves parser locations" "plugin.kpl:2:" \
    plugin doctor --root "$bad_dir"

# --- plugin test ------------------------------------------------------------------
#
# Milestone 3's exit criterion: the bundled cmake-cpp and cargo-rust fixture
# cases must pass without any real build tool being executed. Note there is no
# cmake, cargo, or ninja on the machine running this — that is the point.

expect_status "plugin test runs every bundled plugin's cases" 0 \
    plugin test --root "$repo_root"
expect_stdout_contains "plugin test reports the cmake-cpp build case" \
    "cmake-cpp simple-project.build" plugin test --root "$repo_root"
expect_stdout_contains "plugin test reports the cargo-rust build case" \
    "cargo-rust simple-crate.build" plugin test --root "$repo_root"

expect_status "plugin test accepts a single plugin name" 0 \
    plugin test cargo-rust --root "$repo_root"
expect_empty_stdout "plugin test rejects an unknown plugin name" \
    plugin test no-such-plugin --root "$repo_root"
expect_status "an unknown plugin name exits 1" 1 \
    plugin test no-such-plugin --root "$repo_root"
expect_status "plugin test rejects extra arguments" 2 \
    plugin test a b --root "$repo_root"
expect_status "an unknown plugin subcommand exits 2" 2 plugin frobnicate --root "$repo_root"
expect_stderr_contains "an unknown plugin subcommand names the alternatives" \
    "expected one of: doctor, test" plugin frobnicate --root "$repo_root"

# A plugin whose golden file disagrees with its plugin.kpl must fail loudly and
# show both renderings — "it differs" alone is not actionable.
mkdir -p "$bad_dir/plugins/mismatch/tests/fixtures/demo" \
         "$bad_dir/plugins/mismatch/tests/expected"
cat > "$bad_dir/plugins/mismatch/plugin.kpl" <<'KPL'
manifest { name = "mismatch" version = "1.0.0" api_version = 1 }
command build(project, config, extra) { step echo "actual" }
KPL
printf '{ "steps": [ { "cmd": ["echo", "expected"] } ] }\n' \
    > "$bad_dir/plugins/mismatch/tests/expected/demo.build.steps.json"
touch "$bad_dir/plugins/mismatch/tests/fixtures/demo/.keep"

expect_status "a mismatched golden file exits 1" 1 plugin test mismatch --root "$bad_dir"
expect_stderr_contains "a mismatch shows the actual spec" '"actual"' \
    plugin test mismatch --root "$bad_dir"
expect_stderr_contains "a mismatch shows the expected spec" '"expected"' \
    plugin test mismatch --root "$bad_dir"

# --- malformed config -------------------------------------------------------------

printf 'a = @\n' > "$bad_dir/kap.toml"

expect_status "a malformed config file exits 1" 1 config get --root "$bad_dir" a
expect_stderr_contains "a parse error reports file:line:col" "kap.toml:1:" \
    config get --root "$bad_dir" a

# A config whose section headers are used: the regression that motivated the
# TOML fix. Reading through a header must find the key.
printf '[server]\nhost = "example"\n' > "$bad_dir/kap.toml"
expect_stdout "a key under a section header is reachable" "example" \
    config get --root "$bad_dir" server.host
expect_status "the same key is NOT at the document root" 1 \
    config get --root "$bad_dir" host

# --- summary ----------------------------------------------------------------------

printf '\n%d e2e tests: %d passed, %d failed\n' "$((passed + failed))" "$passed" "$failed"
[ "$failed" -eq 0 ]
