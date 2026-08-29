#!/usr/bin/env sh
# scripts/install.sh
#
# One-command install for kap (design doc Milestone 10):
#
#     curl -fsSL https://raw.githubusercontent.com/kap-project/kap/main/scripts/install.sh | sh
#
# It clones the repository, builds the binary, and installs it together with the
# bundled plugins and the registry index — everything §6.5's third tier expects:
#
#     <prefix>/bin/kap
#     <prefix>/share/kap/plugins/<name>/
#     <prefix>/share/kap/registry/index.toml
#
# kap finds those at run time from its own location, so an installed kap needs
# no environment variables and no configuration to see its own plugins.
#
# ## Why it builds from source
#
# There is no release binary to download yet, and pretending otherwise would
# mean a script that fails on the day someone actually runs it. Building needs a
# C++20 compiler, CMake, and git — all of which the target audience for a
# developer CLI already has, and all of which this script checks for by name
# before it does anything.
#
# POSIX `sh`, not bash: a script piped into `sh` has no say in which shell runs
# it.

set -eu

REPO="${KAP_REPO:-https://github.com/kap-project/kap}"
REF="${KAP_REF:-main}"
PREFIX="${KAP_PREFIX:-$HOME/.local}"
BUILD_TYPE="${KAP_BUILD_TYPE:-Release}"
EMBED="${KAP_EMBED:-0}"

say()  { printf 'kap-install: %s\n' "$1"; }
die()  { printf 'kap-install: error: %s\n' "$1" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    cat <<'USAGE'
usage: install.sh [--prefix DIR] [--ref REF] [--repo URL] [--embed] [--help]

  --prefix DIR   where to install (default: ~/.local, or $KAP_PREFIX)
  --ref REF      branch, tag, or commit to build (default: main)
  --repo URL     repository to clone (default: the kap project)
  --embed        compile the first-party plugins INTO the binary

Environment variables KAP_PREFIX, KAP_REF, KAP_REPO, KAP_BUILD_TYPE, and
KAP_EMBED do the same thing, which is what makes this usable from a `curl | sh`
pipeline where there is nowhere to put a flag:

    curl -fsSL .../install.sh | KAP_EMBED=1 sh

## --embed, and when you want it

Without it, the plugins are installed as text files next to the binary, under
<prefix>/share/kap/plugins. That is the ordinary packaging story, and it is what
a distributor wants: the plugins are files a maintainer can see, diff, and
patch.

With it, the same plugins are compiled into the binary. `kap` becomes one
self-contained file that works with nothing beside it and no network at all —
which is what you want if you are going to copy that file to a server, into a
container, or onto a machine you do not administer.

The failure mode --embed removes is a real one: a `kap` binary lifted out of a
build tree finds no plugins, no registry, and no obvious way to get either.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift 2 ;;
        --prefix=*) PREFIX="${1#--prefix=}"; shift ;;
        --ref)    [ $# -ge 2 ] || die "--ref needs a value";       REF="$2"; shift 2 ;;
        --ref=*)  REF="${1#--ref=}"; shift ;;
        --repo)   [ $# -ge 2 ] || die "--repo needs a URL";        REPO="$2"; shift 2 ;;
        --repo=*) REPO="${1#--repo=}"; shift ;;
        --embed) EMBED=1; shift ;;
        --no-embed) EMBED=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
done

# --- 1. Check the toolchain, all of it, before doing anything ------------------
# Reporting every missing tool at once beats making someone install three of
# them one failed run at a time.
missing=""
for tool in git cmake; do
    have "$tool" || missing="$missing $tool"
done
if ! have c++ && ! have g++ && ! have clang++; then
    missing="$missing a-C++20-compiler(g++-or-clang++)"
fi
[ -z "$missing" ] || die "missing:$missing"

say "repository: $REPO ($REF)"
say "prefix:     $PREFIX"
if [ "$EMBED" = "1" ]; then
    say "plugins:    compiled into the binary (--embed)"
else
    say "plugins:    installed as files under $PREFIX/share/kap/plugins"
fi

# --- 2. Clone into a scratch directory that always gets cleaned up -------------
workdir="$(mktemp -d "${TMPDIR:-/tmp}/kap-install.XXXXXX")"
# The trap covers every exit path including the error ones, so a failed build
# does not leave a few hundred megabytes in /tmp.
trap 'rm -rf "$workdir"' EXIT INT TERM

say "cloning..."
git clone --quiet --depth 1 --branch "$REF" "$REPO" "$workdir/kap" 2>/dev/null \
    || git clone --quiet --depth 1 "$REPO" "$workdir/kap" \
    || die "could not clone $REPO"

if [ "$REF" != "main" ]; then
    # A commit SHA cannot be cloned with --branch, so fetch it explicitly.
    ( cd "$workdir/kap" && git rev-parse --verify --quiet "$REF" >/dev/null ) || {
        say "fetching $REF..."
        ( cd "$workdir/kap" && git fetch --quiet --depth 1 origin "$REF" && git checkout --quiet FETCH_HEAD ) \
            || die "could not check out '$REF'"
    }
fi

# --- 3. Build ------------------------------------------------------------------
generator=""
have ninja && generator="-G Ninja"

embed_flag="-DKAP_EMBED_PLUGINS=OFF"
[ "$EMBED" = "1" ] && embed_flag="-DKAP_EMBED_PLUGINS=ON"

say "building ($BUILD_TYPE)..."
# shellcheck disable=SC2086  # $generator is deliberately word-split or empty
cmake -S "$workdir/kap" -B "$workdir/build" $generator \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      "$embed_flag" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null \
    || die "cmake configure failed"
cmake --build "$workdir/build" >/dev/null || die "build failed"

# --- 4. Verify before installing ------------------------------------------------
# A binary that does not pass its own tests should not be put on someone's PATH.
say "checking..."
ctest --test-dir "$workdir/build" --output-on-failure >/dev/null 2>&1 \
    || die "the test suite did not pass; refusing to install"

# --- 5. Install -----------------------------------------------------------------
say "installing..."
cmake --install "$workdir/build" --prefix "$PREFIX" >/dev/null || die "install failed"

installed="$PREFIX/bin/kap"
[ -x "$installed" ] || die "expected a binary at $installed"

version="$("$installed" --version)"
say "installed $version -> $installed"

# --- 6. Tell the user what is left to do ----------------------------------------
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *)
        printf '\n'
        say "$PREFIX/bin is not on your PATH. Add it:"
        printf '\n    export PATH="%s/bin:$PATH"\n\n' "$PREFIX"
        ;;
esac

case "$REPO" in
    http://*|https://*) docs_location="$REPO/tree/$REF/docs" ;;
    *)                  docs_location="the docs/ directory of your checkout" ;;
esac

cat <<NEXT

Next:

    kap detect                     which plugin claims this directory
    kap doctor                     are the tools it needs installed
    kap build                      build it
    kap build -n                   ...or just show what that would run

    kap plugin list                the bundled plugins ($("$installed" plugin list | wc -l | tr -d ' ') available)
    kap plugin new my-ecosystem    write your own

    kap help                       every command, with a page for each
    kap <command> --help           e.g. kap install --help

    kap completions bash > ~/.local/share/bash-completion/completions/kap

Docs: $docs_location
NEXT
