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

# Hermeticity: kap reads ~/.config/kap/config.toml as the global configuration
# layer (§5.12) and writes ~/.local/share and ~/.cache. A developer with real
# files in any of those would get different results from CI, which is the exact
# class of flake that makes a suite stop being trusted. Point all three at a
# scratch directory for the duration of the run.
e2e_home="$(mktemp -d)"
export XDG_CONFIG_HOME="$e2e_home/config"
export XDG_DATA_HOME="$e2e_home/data"
export XDG_CACHE_HOME="$e2e_home/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"

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
expect_stderr_contains "a missing config file says so" "no configuration file" \
    config get --root /nonexistent-directory-xyz some.key
expect_stderr_contains "a missing config file names where it looked" "config.toml" \
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

# Milestone 8: all six first-party ecosystems ship with passing fixture cases,
# and none of them needs its ecosystem toolchain to be installed.
for plugin_name in cmake-cpp cargo-rust make-generic node go python-uv doctor ports; do
    expect_status "plugin test passes for $plugin_name" 0 \
        plugin test "$plugin_name" --root "$repo_root"
    expect_status "plugin doctor passes for $plugin_name" 0 \
        plugin doctor "$plugin_name" --root "$repo_root"
done

# The node plugin's workspace `dev` is the only bundled case that produces a
# concurrent spec with per-step cwd and label, so it is worth asserting through
# the binary and not only through the golden file.
expect_stdout_contains "the node workspace dev case is exercised" \
    "node workspace.dev" plugin test node --root "$repo_root"

expect_status "plugin test accepts a single plugin name" 0 \
    plugin test cargo-rust --root "$repo_root"
expect_empty_stdout "plugin test rejects an unknown plugin name" \
    plugin test no-such-plugin --root "$repo_root"
expect_status "an unknown plugin name exits 1" 1 \
    plugin test no-such-plugin --root "$repo_root"
# `plugin test` takes several names, so two arguments mean two plugins — and
# a name that is neither installed nor a directory is an error, not a silent
# no-op.
expect_status "plugin test names both bundled plugins" 0 \
    plugin test cargo-rust cmake-cpp --root "$repo_root"
expect_status "plugin test rejects an unknown name among several" 1 \
    plugin test cargo-rust no-such-plugin --root "$repo_root"

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

# --- detect (Milestone 4) ---------------------------------------------------------
#
# The detection engine has thorough unit tests in tests/test_detect.cpp. What
# only the real binary can show is the *user-visible* contract: a project that
# matches exits 0 and names the plugin on stdout, a directory that matches
# nothing exits 1 with an explanation on stderr, and the cache line flips from
# "miss" to "hit" on a second run.

detect_dir="$(mktemp -d)"
mkdir -p "$detect_dir/proj"
printf 'cmake_minimum_required(VERSION 3.16)\n' > "$detect_dir/proj/CMakeLists.txt"
export KAP_PLUGIN_PATH="$repo_root/kap-plugins"

expect_status "detect exits 0 in a project it recognises" 0 detect --root "$detect_dir/proj"
expect_stdout_contains "detect names the winning plugin" "cmake-cpp" \
    detect --root "$detect_dir/proj"
expect_stdout_contains "detect reports the marker that fired" "CMakeLists.txt" \
    detect --root "$detect_dir/proj"
expect_stdout_contains "detect reports the priority and score" "priority=30 score=1" \
    detect --root "$detect_dir/proj"

# First run scans; the entry it leaves behind makes the second run a hit.
"$kap_bin" detect --root "$detect_dir/proj" >/dev/null 2>&1
expect_stdout_contains "a second detect run hits the cache" "cache:  hit" \
    detect --root "$detect_dir/proj"
expect_stdout_contains "--refresh forces a rescan" "cache:  miss" \
    detect --refresh --root "$detect_dir/proj"

if [ -f "$detect_dir/proj/.kap/cache.json" ]; then
    pass "detect writes .kap/cache.json"
else
    fail "detect writes .kap/cache.json" "no cache file under $detect_dir/proj/.kap"
fi
if grep -q '^\*$' "$detect_dir/proj/.kap/.gitignore" 2>/dev/null; then
    pass "the cache directory ignores itself"
else
    fail "the cache directory ignores itself" "no self-ignoring .kap/.gitignore"
fi

# Deleting the marker must change the answer, not serve a stale one.
rm -f "$detect_dir/proj/CMakeLists.txt"
expect_status "removing the marker makes detection fail again" 1 detect --root "$detect_dir/proj"

mkdir -p "$detect_dir/empty"
expect_status "detect exits 1 where nothing matches" 1 detect --root "$detect_dir/empty"
expect_empty_stdout "a failed detect writes nothing to stdout" detect --root "$detect_dir/empty"
expect_stderr_contains "a failed detect says which plugins were considered" "considered" \
    detect --root "$detect_dir/empty"

# A tie is refused rather than guessed at (design doc §3.2 step 4), and the
# error has to be actionable enough to fix without reading the design doc.
tie_plugins="$detect_dir/tie-plugins"
for name in alpha beta; do
    mkdir -p "$tie_plugins/$name"
    cat > "$tie_plugins/$name/plugin.kpl" <<KPL
manifest { name = "$name" version = "1.0.0" api_version = 1 priority = 25 }
detect { file_exists "shared.txt" }
KPL
done
mkdir -p "$detect_dir/tie"
printf 'x\n' > "$detect_dir/tie/shared.txt"

KAP_PLUGIN_PATH="$tie_plugins" "$kap_bin" detect --root "$detect_dir/tie" >/dev/null 2>&1
if [ $? -eq 1 ]; then
    pass "a detection tie exits 1"
else
    fail "a detection tie exits 1" "expected exit status 1"
fi
tie_err="$(KAP_PLUGIN_PATH="$tie_plugins" "$kap_bin" detect --root "$detect_dir/tie" 2>&1 >/dev/null)"
case "$tie_err" in
    *alpha*beta*ecosystem*) pass "a tie names both plugins and the fix" ;;
    *) fail "a tie names both plugins and the fix" "got: $tie_err" ;;
esac

# A pin in kap.toml settles it without an error.
printf '[detect]\necosystem = "beta"\n' > "$detect_dir/tie/kap.toml"
tie_out="$(KAP_PLUGIN_PATH="$tie_plugins" "$kap_bin" detect --refresh --root "$detect_dir/tie" 2>/dev/null)"
case "$tie_out" in
    beta*) pass "a kap.toml pin settles a tie" ;;
    *) fail "a kap.toml pin settles a tie" "got: $tie_out" ;;
esac

expect_status "detect rejects an unknown option" 2 detect --nonsense --root "$detect_dir/proj"

unset KAP_PLUGIN_PATH
rm -rf "$detect_dir"

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

# --- config set / edit (Milestone 6) ----------------------------------------------

set_dir="$(mktemp -d)"

expect_status "config set writes a project key" 0 \
    config set --root "$set_dir" plugins.cmake-cpp.generator ninja
expect_stdout "config set reads back through config get" "ninja" \
    config get --root "$set_dir" plugins.cmake-cpp.generator
expect_status "config set with the wrong number of arguments exits 2" 2 \
    config set --root "$set_dir" only-a-key
expect_status "config set rejects an unknown option" 2 \
    config set --root "$set_dir" --nonsense a b

# --dry-run must not touch the file.
expect_stdout_contains "config set honours --dry-run" "would set" \
    -n config set --root "$set_dir" plugins.cmake-cpp.generator make
expect_stdout "config set --dry-run really did not write" "ninja" \
    config get --root "$set_dir" plugins.cmake-cpp.generator

# The global layer is a separate file, and the project layer wins over it.
"$kap_bin" config set --global plugins.cmake-cpp.build_dir global-dir >/dev/null 2>&1
expect_stdout "config get --global reads the global file" "global-dir" \
    config get --root "$set_dir" --global plugins.cmake-cpp.build_dir
"$kap_bin" config set --root "$set_dir" plugins.cmake-cpp.build_dir project-dir >/dev/null 2>&1
expect_stdout "the project layer wins in the merged view" "project-dir" \
    config get --root "$set_dir" plugins.cmake-cpp.build_dir
expect_stdout "the global layer still shows its own value" "global-dir" \
    config get --root "$set_dir" --global plugins.cmake-cpp.build_dir

# config edit needs an editor and says so rather than guessing at one.
saved_editor="${EDITOR:-}"; saved_visual="${VISUAL:-}"
unset EDITOR VISUAL
expect_status "config edit without an editor exits 1" 1 config edit --root "$set_dir"
expect_stderr_contains "config edit names the file when it cannot open it" "kap.toml" \
    config edit --root "$set_dir"
EDITOR=true "$kap_bin" config edit --root "$set_dir" >/dev/null 2>&1
if [ $? -eq 0 ]; then
    pass "config edit runs \$EDITOR"
else
    fail "config edit runs \$EDITOR" "expected exit status 0"
fi
[ -n "$saved_editor" ] && export EDITOR="$saved_editor"
[ -n "$saved_visual" ] && export VISUAL="$saved_visual"

# The global layer written above is real and would otherwise change every test
# below it — which is itself proof the layer works, but not what those tests are
# checking. Reset it.
rm -f "$XDG_CONFIG_HOME/kap/config.toml"
rm -rf "$set_dir"

# --- the project-command lifecycle (Milestones 5 + 6) ------------------------------
#
# Milestone 5 and 6's shared exit criterion: `kap build` builds a real CMake
# project and `kap build -n` prints the commands without running them. This is
# the only place the whole chain — CLI, config merge, detection, plugin load,
# type check, KPL evaluation, executor — is exercised as one thing.

proj="$(mktemp -d)"
export KAP_PLUGIN_PATH="$repo_root/kap-plugins"
cat > "$proj/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(kap_e2e_demo CXX)
add_executable(demo main.cpp)
CMAKE
cat > "$proj/main.cpp" <<'CPP'
#include <cstdio>
int main() { std::puts("built by kap"); return 0; }
CPP

expect_status "build --dry-run exits 0" 0 build -n --root "$proj"
expect_stdout_contains "build --dry-run shows the configure step" "cmake -S . -B build" \
    build -n --root "$proj"
expect_stdout_contains "build --dry-run shows the build step" "cmake --build build" \
    build -n --root "$proj"
if [ -d "$proj/build" ]; then
    fail "build --dry-run runs nothing" "a build directory was created"
else
    pass "build --dry-run runs nothing"
fi

expect_stdout_contains "--set reaches the plugin's config record" "cmake -S . -B out" \
    build -n --root "$proj" --set build_dir=out
expect_stdout_contains "--set appends list values" "-DFROM_SET=1" \
    build -n --root "$proj" --set cmake_args=-DFROM_SET=1

expect_status "--set with an unknown key exits 1" 1 \
    build -n --root "$proj" --set no_such_key=1
expect_stderr_contains "an unknown key names itself" "unknown config key 'no_such_key'" \
    build -n --root "$proj" --set no_such_key=1
expect_status "--set with a bad enum member exits 1" 1 \
    build -n --root "$proj" --set generator=clown
expect_stderr_contains "a bad enum member lists the valid ones" "auto, ninja, make" \
    build -n --root "$proj" --set generator=clown

# kap.toml overrides, and hooks (§5.13).
cat > "$proj/kap.toml" <<'TOML'
[plugins.cmake-cpp]
build_dir = "out"

[hooks]
pre_build = "echo HOOK-PRE"
post_build = "echo HOOK-POST"
TOML
expect_stdout_contains "kap.toml overrides reach the plugin" "cmake -S . -B out" \
    build -n --root "$proj"
expect_stdout_contains "--dry-run shows the pre hook" "pre_build" build -n --root "$proj"

# The real thing. Skipped where cmake is not installed — CI's dev image has it,
# a bare machine may not, and a missing toolchain is not a kap failure.
if command -v cmake >/dev/null 2>&1; then
    build_log="$("$kap_bin" build --root "$proj" 2>&1)"
    build_status=$?
    if [ "$build_status" -eq 0 ] && [ -x "$proj/out/demo" ]; then
        pass "kap build builds a real CMake project"
    else
        fail "kap build builds a real CMake project" "status=$build_status log=$build_log"
    fi
    case "$build_log" in
        *HOOK-PRE*HOOK-POST*) pass "hooks run before and after the build" ;;
        *) fail "hooks run before and after the build" "log=$build_log" ;;
    esac
    if [ "$("$proj/out/demo")" = "built by kap" ]; then
        pass "the binary kap built actually runs"
    else
        fail "the binary kap built actually runs" "unexpected output"
    fi

    # clean removes it and reports what it recovered.
    clean_log="$("$kap_bin" clean --root "$proj" 2>&1)"
    if [ ! -d "$proj/out" ]; then
        pass "kap clean removes the build directory"
    else
        fail "kap clean removes the build directory" "$proj/out still exists"
    fi
    case "$clean_log" in
        *freed*) pass "kap clean reports the space it freed" ;;
        *) fail "kap clean reports the space it freed" "log=$clean_log" ;;
    esac

    # A failing tool's exit code is propagated, never swallowed (§4 step 7).
    printf 'this is not valid cmake(((\n' > "$proj/CMakeLists.txt"
    rm -rf "$proj/out"
    "$kap_bin" build --root "$proj" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        pass "a failing build propagates a non-zero exit code"
    else
        fail "a failing build propagates a non-zero exit code" "expected non-zero"
    fi
else
    printf '[SKIP] kap build against a real CMake project (cmake not installed)\n'
fi

# A command the matched plugin does not define is refused with the list it does.
expect_status "an undefined command exits 1" 1 dev --root "$proj"
expect_stderr_contains "an undefined command lists what is available" "available:" \
    dev --root "$proj"

# Tool arguments go after `--`, and a bare one is refused rather than guessed at.
expect_status "a bare tool argument is refused" 2 build --root "$proj" --target install
expect_stderr_contains "the refusal shows the '--' form" "kap build -- --target install" \
    build --root "$proj" --target install
expect_stdout_contains "passthrough arguments reach the build step" "--target install" \
    build -n --root "$proj" -- --target install

# Nothing matches: a clear error, not a crash and not a guess.
empty_proj="$(mktemp -d)"
expect_status "a directory no plugin claims exits 1" 1 build --root "$empty_proj"
expect_stderr_contains "it says nothing owns the directory" "no plugin claims" \
    build --root "$empty_proj"
expect_stderr_contains "it says which plugins were considered" "considered" \
    build --root "$empty_proj"
rm -rf "$empty_proj"

# An unknown command lists what kap does know.
expect_status "an unknown command exits 2" 2 buidl
expect_stderr_contains "an unknown command lists the project commands" "project commands:" buidl

unset KAP_PLUGIN_PATH
rm -rf "$proj"

# --- the bundled system plugins (Milestone 9) --------------------------------------
#
# `doctor` and `ports` are written entirely in KPL (design doc §4 and §6.6), so
# what has to be checked here is that the core's half of the arrangement works:
# it collects every matched plugin's `requires` block, injects it, and the
# plugin decides the rest.

sys_dir="$(mktemp -d)"
printf 'cmake_minimum_required(VERSION 3.16)\n' > "$sys_dir/CMakeLists.txt"
export KAP_PLUGIN_PATH="$repo_root/kap-plugins"

expect_stdout_contains "doctor names the plugins it checked" "cmake-cpp" \
    doctor --root "$sys_dir"
expect_stdout_contains "doctor reports the tools cmake-cpp requires" "cmake" \
    doctor --root "$sys_dir"
expect_stdout_contains "doctor marks optional tools as optional" "optional" \
    doctor --root "$sys_dir"

# The injection is the whole Milestone-9 mechanism: a tool no plugin could
# possibly have declared must be reportable purely by setting the config key.
expect_stdout_contains "an injected required tool that is absent is reported MISSING" \
    "kap-no-such-tool-xyz  required  MISSING" \
    doctor --root "$sys_dir" --set required_tools=kap-no-such-tool-xyz
expect_status "doctor exits non-zero when a required tool is missing" 1 \
    doctor --root "$sys_dir" --set required_tools=kap-no-such-tool-xyz

# `any_of` grouping: one installed alternative satisfies the whole group.
#
# Written through kap.toml rather than --set, because --set splits a list value
# on commas — so `--set required_tools=a,b` means two groups of one, not one
# group of two. A group has to arrive as a single string, which a TOML array
# element can be and a --set element cannot.
cat > "$sys_dir/kap.toml" <<'TOML'
[plugins.doctor]
required_tools = ["kap-nope,sh"]
TOML
expect_stdout_contains "an any_of group is satisfied by one member" "ok  sh" \
    doctor --root "$sys_dir"
expect_status "a satisfied any_of group exits 0" 0 doctor --root "$sys_dir"

printf '[plugins.doctor]\nrequired_tools = ["kap-nope,kap-also-nope"]\n' > "$sys_dir/kap.toml"
expect_stdout_contains "an unsatisfiable group says any one would do" "need any one of these" \
    doctor --root "$sys_dir"
expect_status "an unsatisfiable group exits 1" 1 doctor --root "$sys_dir"
rm -f "$sys_dir/kap.toml"

# ports resolves to a real command and shows it under --dry-run without running.
expect_status "ports --dry-run exits 0" 0 ports -n --root "$sys_dir"
ports_dry="$("$kap_bin" ports -n --root "$sys_dir" 2>/dev/null)"
case "$ports_dry" in
    *ss*|*lsof*|*netstat*) pass "ports resolves to one of ss, lsof, or netstat" ;;
    *) fail "ports resolves to one of ss, lsof, or netstat" "got: $ports_dry" ;;
esac
expect_stdout_contains "ports honours an explicit tool choice" "lsof" \
    ports -n --root "$sys_dir" --set tool=lsof
expect_stdout_contains "ports honours its flags" "-u" \
    ports -n --root "$sys_dir" --set tool=ss --set udp=true

# The sidecars claim every directory, so a directory with no *owner* must still
# say so rather than complaining about a missing command.
sidecar_only="$(mktemp -d)"
expect_status "a directory only sidecars claim still fails a build" 1 \
    build --root "$sidecar_only"
expect_stderr_contains "and explains that nothing owns it" "no plugin claims" \
    build --root "$sidecar_only"
expect_status "but doctor still works there" 0 doctor --root "$sidecar_only"
rm -rf "$sidecar_only"

unset KAP_PLUGIN_PATH
rm -rf "$sys_dir"

# --- the plugin manager (Milestone 7) ---------------------------------------------
#
# The library-level pipeline is covered in tests/test_registry.cpp. What only
# the binary can show is the command surface: which exit codes each subcommand
# produces, that `install` really refuses without confirmation, and that the
# whole author loop — new, doctor, test, install --link, list, disable, remove
# — works from a shell.

pm_dir="$(mktemp -d)"

expect_status "plugin with no subcommand shows usage and exits 2" 2 plugin
expect_stderr_contains "plugin usage lists the subcommands" "install <name|url|path>" plugin
expect_status "an unknown plugin subcommand exits 2" 2 plugin frobnicate
expect_stderr_contains "an unknown plugin subcommand names the alternatives" \
    "list search install" plugin frobnicate
expect_status "plugin doctor accepts several names at once" 0 \
    plugin doctor cargo-rust cmake-cpp --root "$repo_root"

# kap plugin new, then the two commands its own output tells you to run.
expect_status "plugin new scaffolds a plugin" 0 plugin new demo-plugin --root "$pm_dir"
if [ -f "$pm_dir/demo-plugin/plugin.kpl" ] && [ -f "$pm_dir/demo-plugin/README.md" ]; then
    pass "plugin new writes plugin.kpl and README.md"
else
    fail "plugin new writes plugin.kpl and README.md" "missing files under $pm_dir/demo-plugin"
fi
expect_status "plugin new refuses to overwrite" 1 plugin new demo-plugin --root "$pm_dir"
expect_status "plugin new rejects an unknown template" 1 \
    plugin new other --template nonsense --root "$pm_dir"

# doctor and test take a *path*, which is what a plugin author has before they
# have installed anything.
expect_status "plugin doctor accepts a directory" 0 plugin doctor demo-plugin --root "$pm_dir"
expect_stdout_contains "plugin doctor passes the scaffold" "[PASS] demo-plugin" \
    plugin doctor demo-plugin --root "$pm_dir"
expect_status "plugin test accepts a directory" 0 plugin test demo-plugin --root "$pm_dir"
expect_stdout_contains "the scaffold ships a passing case" "1 passed" \
    plugin test demo-plugin --root "$pm_dir"
expect_status "plugin doctor rejects a path that is not a plugin" 1 \
    plugin doctor no-such-directory --root "$pm_dir"

# install --link, then the lockfile-backed state commands.
expect_status "install --link --yes succeeds" 0 \
    plugin install --link --yes "$pm_dir/demo-plugin"
expect_stdout_contains "the installed plugin is listed" "demo-plugin" plugin list
expect_stdout_contains "list shows where it came from" "link" plugin list

expect_status "disable succeeds" 0 plugin disable demo-plugin
expect_stdout_contains "a disabled plugin is marked" "disabled" plugin list
expect_status "enable succeeds" 0 plugin enable demo-plugin

expect_status "pin succeeds" 0 plugin pin demo-plugin 0.1.0
expect_stdout_contains "a pinned plugin shows its pin" "pinned=0.1.0" plugin list
expect_status "update refuses a pinned plugin" 1 plugin update demo-plugin
expect_stderr_contains "the refusal explains how to unpin" "--clear" plugin update demo-plugin
expect_status "unpinning succeeds" 0 plugin pin demo-plugin --clear

# §7's confirmation. stdin is closed, so a prompt must fail rather than proceed.
"$kap_bin" plugin install --force "$pm_dir/demo-plugin" </dev/null >/dev/null 2>&1
if [ $? -ne 0 ]; then
    pass "install without --yes refuses when it cannot ask"
else
    fail "install without --yes refuses when it cannot ask" "expected a non-zero exit"
fi

expect_status "remove succeeds" 0 plugin remove demo-plugin
expect_status "removing it twice exits 1" 1 plugin remove demo-plugin
if [ -f "$pm_dir/demo-plugin/plugin.kpl" ]; then
    pass "removing a linked plugin leaves the working copy alone"
else
    fail "removing a linked plugin leaves the working copy alone" "the source was deleted"
fi

# search reads the repository's own registry index.
expect_status "search finds a bundled plugin" 0 plugin search cmake --root "$repo_root"
expect_stdout_contains "search reports the match" "cmake-cpp" plugin search cmake --root "$repo_root"
expect_status "search with no match exits 1" 1 \
    plugin search zzz-no-such-plugin --root "$repo_root"
expect_status "search with no query exits 2" 2 plugin search --root "$repo_root"

expect_status "install --bundle with an unknown bundle exits 1" 1 \
    plugin install --yes --bundle no-such-bundle --root "$repo_root"
expect_stderr_contains "an unknown bundle lists the real ones" "available:" \
    plugin install --yes --bundle no-such-bundle --root "$repo_root"

# --dry-run must not touch anything.
expect_stdout_contains "install honours --dry-run" "would install" \
    -n plugin install --yes "$pm_dir/demo-plugin"
expect_stdout_contains "plugin new honours --dry-run" "would scaffold" \
    -n plugin new dry-plugin --root "$pm_dir"
if [ -d "$pm_dir/dry-plugin" ]; then
    fail "plugin new --dry-run creates nothing" "dry-plugin was created"
else
    pass "plugin new --dry-run creates nothing"
fi

rm -rf "$pm_dir"

# --- shell completions (Milestone 10) ----------------------------------------------
#
# The unit tests check the scripts mention the right words. Only here can they
# be handed to a real shell, which is the assertion that actually matters: a
# completion script with a syntax error is worse than none, because it makes
# every Tab in that shell print an error.

comp_dir="$(mktemp -d)"

expect_status "completions with no shell exits 2" 2 completions
expect_status "completions with an unknown shell exits 1" 1 completions csh
expect_stderr_contains "an unknown shell lists the real ones" "bash zsh fish" completions csh

for shell_name in bash zsh fish; do
    expect_status "completions $shell_name exits 0" 0 completions "$shell_name"
    "$kap_bin" completions "$shell_name" > "$comp_dir/$shell_name" 2>/dev/null
    if [ -s "$comp_dir/$shell_name" ]; then
        pass "completions $shell_name writes a non-empty script"
    else
        fail "completions $shell_name writes a non-empty script" "empty output"
    fi
done

# bash is always present (this script is running under it).
if bash -n "$comp_dir/bash" 2>/dev/null; then
    pass "the bash completion script parses"
else
    fail "the bash completion script parses" "$(bash -n "$comp_dir/bash" 2>&1)"
fi

# Sourcing it and driving the completion function is the real end-to-end check.
completion_out="$(bash -c "source '$comp_dir/bash'; COMP_WORDS=(kap plugin ins); COMP_CWORD=2; _kap; printf '%s\n' \"\${COMPREPLY[@]}\"" 2>/dev/null)"
if [ "$completion_out" = "install" ]; then
    pass "the bash completion actually completes a subcommand"
else
    fail "the bash completion actually completes a subcommand" "got '$completion_out'"
fi

if command -v zsh >/dev/null 2>&1; then
    if zsh -n "$comp_dir/zsh" 2>/dev/null; then
        pass "the zsh completion script parses"
    else
        fail "the zsh completion script parses" "$(zsh -n "$comp_dir/zsh" 2>&1)"
    fi
else
    printf '[SKIP] the zsh completion script parses (zsh not installed)\n'
fi

if command -v fish >/dev/null 2>&1; then
    if fish -n "$comp_dir/fish" 2>/dev/null; then
        pass "the fish completion script parses"
    else
        fail "the fish completion script parses" "$(fish -n "$comp_dir/fish" 2>&1)"
    fi
else
    printf '[SKIP] the fish completion script parses (fish not installed)\n'
fi

rm -rf "$comp_dir"

# --- kap dev -o (Milestone 10) -------------------------------------------------------

dev_dir="$(mktemp -d)"
export KAP_PLUGIN_PATH="$repo_root/kap-plugins"
cat > "$dev_dir/package.json" <<'JSON'
{
  "name": "kap-e2e-dev",
  "private": true,
  "scripts": { "dev": "echo Local: http://127.0.0.1:59999/" }
}
JSON

# -o is a `dev` option, so it must be accepted there...
expect_status "dev accepts -o" 0 dev -n --root "$dev_dir" -o
expect_status "dev accepts --open" 0 dev -n --root "$dev_dir" --open
# ...and refused everywhere else, since it means nothing for a one-shot command.
expect_status "build refuses -o" 2 build -n --root "$dev_dir" -o

rm -rf "$dev_dir"
unset KAP_PLUGIN_PATH

# --- per-command help ---------------------------------------------------------------
#
# `kap install -h` used to print the global banner: the one place someone is
# already confused, answered with a page that says nothing about what they
# asked. Every command now has its own page.

for cmd in build check ci clean dev doctor fmt install lint ports run test \
           detect config plugin completions help; do
    expect_status "kap $cmd --help exits 0" 0 "$cmd" --help
    expect_stdout_contains "kap $cmd --help is about $cmd" "kap $cmd" "$cmd" --help
done

expect_status "the short form works too" 0 install -h
expect_stdout_contains "install's page explains both meanings" "Install a PLUGIN" install -h
expect_stdout_contains "dev's page states what -o costs" "colours" dev --help

# Subcommand pages.
expect_stdout_contains "plugin install has its own page" "kap plugin install" \
    plugin install --help
expect_stdout_contains "plugin new has its own page" "kap plugin new" plugin new --help
expect_stdout_contains "plugin disable has its own page" "kap plugin disable" \
    plugin disable --help

# `kap help` as a command.
expect_status "kap help exits 0" 0 help
expect_stdout_contains "kap help lists the commands" "build" help
expect_stdout_contains "kap help lists the subcommands" "plugin install" help
expect_stdout_contains "kap help <command> works" "kap plugin install" help plugin install
expect_status "kap help with an unknown topic exits 1" 1 help frobnicate

# --help after `--` belongs to the tool, not to kap.
help_dir="$(mktemp -d)"
printf 'cmake_minimum_required(VERSION 3.16)\n' > "$help_dir/CMakeLists.txt"
export KAP_PLUGIN_PATH="$repo_root/kap-plugins"
expect_stdout_contains "--help after -- reaches the tool" "--help" \
    build -n --root "$help_dir" -- --help

# --- `kap install <name>` (the trap, removed) ---------------------------------------
#
# `install` is a project command, but everyone's instinct is "install a plugin".
# It used to be an error whose advice made it worse: it suggested
# `kap install -- cmake-cpp`, a different wrong thing.

expect_stderr_contains "kap install <name> says what it is doing" \
    "means 'kap plugin install" install no-such-plugin-xyz --root "$help_dir"
expect_stderr_contains "and how to get the project command" "installs the project itself" \
    install no-such-plugin-xyz --root "$help_dir"
expect_status "an unknown plugin name still fails" 1 \
    install no-such-plugin-xyz --root "$help_dir"

# --- a machine with no plugins gets a diagnosis, not a dead end ----------------------

bare_dir="$(mktemp -d)"
printf 'cmake_minimum_required(VERSION 3.16)\n' > "$bare_dir/CMakeLists.txt"
unset KAP_PLUGIN_PATH
bare_home="$(mktemp -d)"
# KAP_NO_EMBEDDED_PLUGINS makes this reachable on a -DKAP_EMBED_PLUGINS=ON
# build too, where every directory otherwise has a plugin — which is the point
# of that build, and would make this assertion untestable without the switch.
bare_out="$(HOME="$bare_home" XDG_DATA_HOME="$bare_home/data" \
            XDG_CONFIG_HOME="$bare_home/config" XDG_CACHE_HOME="$bare_home/cache" \
            KAP_BUNDLED_PLUGIN_DIR="$bare_home/nowhere" KAP_NO_EMBEDDED_PLUGINS=1 \
            "$kap_bin" build --root "$bare_dir" 2>&1)"
case "$bare_out" in
    *"kap looked in"*) pass "no plugins names every directory searched" ;;
    *) fail "no plugins names every directory searched" "got: $bare_out" ;;
esac
case "$bare_out" in
    *"kap plugin install --bundle core"*) pass "and says how to fix it" ;;
    *) fail "and says how to fix it" "got: $bare_out" ;;
esac
rm -rf "$bare_home" "$bare_dir"

# --- the registry index is always available ------------------------------------------
#
# It is compiled into the binary, so search and name resolution work even with
# nothing on disk. Before, a copied binary answered `kap plugin search` with
# "no registry index found" and had no way forward.

search_home="$(mktemp -d)"
search_out="$(HOME="$search_home" XDG_DATA_HOME="$search_home/data" \
              XDG_CONFIG_HOME="$search_home/config" XDG_CACHE_HOME="$search_home/cache" \
              KAP_BUNDLED_PLUGIN_DIR="$search_home/nowhere" KAP_NO_EMBEDDED_PLUGINS=1 \
              "$kap_bin" plugin search cmake --root "$search_home" 2>&1)"
case "$search_out" in
    *cmake-cpp*) pass "plugin search works with nothing on disk" ;;
    *) fail "plugin search works with nothing on disk" "got: $search_out" ;;
esac
rm -rf "$search_home"

# --- installing from an installer-script URL ------------------------------------------
#
# The route a third-party plugin uses. Served from loopback, which is the one
# place kap allows plain http — everywhere else an installer URL must be https,
# because kap runs what it downloads.

expect_status "a plain-http installer URL is refused" 1 \
    plugin install --yes http://example.invalid/install.sh
expect_stderr_contains "and says why" "tampered with in transit" \
    plugin install --yes http://example.invalid/install.sh
expect_stderr_contains "and what to do instead" "use an https:// address" \
    plugin install --yes http://example.invalid/install.sh

if command -v python3 >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
    script_home="$(mktemp -d)"
    mkdir -p "$script_home/www"
    cat > "$script_home/www/install.sh" <<'INSTALLER'
#!/bin/sh
# A third-party plugin installer, exactly as docs/plugins.md describes one.
set -eu
cat > "$KAP_PLUGIN_DEST/plugin.kpl" <<'KPL'
manifest { name = "e2e-zig" version = "0.3.0" api_version = 1 priority = 35 }
detect { file_exists "build.zig" }
requires { any_of [zig] }
schema { release: bool = false }
command build(project, config, extra) {
  let flags = if config.release then ["-Doptimize=ReleaseSafe"] else []
  step ["zig", "build"] + flags + extra
}
KPL
INSTALLER
    ( cd "$script_home/www" && exec python3 -m http.server 8791 >/dev/null 2>&1 ) &
    server_pid=$!

    # Wait for the port to answer rather than sleeping a fixed amount. A fixed
    # sleep is either too short on a loaded machine (a flaky test) or too long
    # on every other run (a slow suite).
    server_ready=0
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        if curl -fsS -o /dev/null "http://127.0.0.1:8791/install.sh" 2>/dev/null; then
            server_ready=1
            break
        fi
        sleep 0.25
    done

    if [ "$server_ready" -ne 1 ]; then
        printf '[SKIP] installer-script installs (local server did not start)\n'
    else
    install_log="$(HOME="$script_home" XDG_DATA_HOME="$script_home/data" \
       XDG_CACHE_HOME="$script_home/cache" XDG_CONFIG_HOME="$script_home/config" \
       "$kap_bin" plugin install --yes http://127.0.0.1:8791/install.sh 2>&1)"
    if [ $? -eq 0 ]; then
        pass "a plugin installs from an installer-script URL"
    else
        fail "a plugin installs from an installer-script URL" "$install_log"
    fi

    listed="$(HOME="$script_home" XDG_DATA_HOME="$script_home/data" \
              XDG_CACHE_HOME="$script_home/cache" XDG_CONFIG_HOME="$script_home/config" \
              "$kap_bin" plugin list 2>/dev/null)"
    case "$listed" in
        *e2e-zig*script*) pass "and is recorded with a script origin" ;;
        *) fail "and is recorded with a script origin" "got: $listed" ;;
    esac

    # The payload a script produces is validated like any other: a script that
    # writes rubbish must not be able to install it.
    cat > "$script_home/www/bad.sh" <<'INSTALLER'
#!/bin/sh
printf 'manifest { name = \n' > "$KAP_PLUGIN_DEST/plugin.kpl"
INSTALLER
    if HOME="$script_home" XDG_DATA_HOME="$script_home/data" \
       XDG_CACHE_HOME="$script_home/cache" XDG_CONFIG_HOME="$script_home/config" \
       "$kap_bin" plugin install --yes http://127.0.0.1:8791/bad.sh >/dev/null 2>&1; then
        fail "a script producing a broken plugin is refused" "the install succeeded"
    else
        pass "a script producing a broken plugin is refused"
    fi

    fi
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    rm -rf "$script_home"
else
    printf '[SKIP] installer-script installs (python3 or curl not available)\n'
fi

rm -rf "$help_dir"

# --- summary ----------------------------------------------------------------------

rm -rf "$e2e_home"

printf '\n%d e2e tests: %d passed, %d failed\n' "$((passed + failed))" "$passed" "$failed"
[ "$failed" -eq 0 ]
