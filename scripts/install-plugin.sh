#!/usr/bin/env sh
# scripts/install-plugin.sh
#
# Installs one first-party kap plugin by downloading its files over HTTPS.
#
# Two ways in.
#
# **kap runs it.** `kap plugin install cmake-cpp` fetches this script (the
# registry index names it) and runs it in a staging directory, with
# KAP_PLUGIN_DEST pointing there. kap then validates whatever lands and installs
# it. This script writes files; it decides nothing.
#
# **You run it.** For a machine with no kap yet, or to install into an explicit
# directory:
#
#     curl -fsSL https://raw.githubusercontent.com/kap-project/kap/main/scripts/install-plugin.sh \
#       | sh -s -- cmake-cpp
#
#     # or all of them
#     curl -fsSL .../install-plugin.sh | sh -s -- --all
#
# ## Why HTTPS and not git
#
# A plugin is two text files. Cloning a repository to get them needs git
# installed, downloads everything else in the repository, and fails on a machine
# behind a proxy that allows HTTPS but not the git protocol. `curl` is enough.
#
# ## The environment kap provides
#
#     KAP_PLUGIN_DEST   where to write; the plugin's files go directly here
#     KAP_PLUGIN_NAME   the plugin kap was asked for
#     KAP_VERSION       the running kap's version
#
# When run by hand, --dest and the positional name take their place.
#
# POSIX sh: a script piped into `sh` has no say in which shell runs it.

set -eu

REPO_RAW="${KAP_PLUGIN_SOURCE:-https://raw.githubusercontent.com/kap-project/kap}"
REF="${KAP_PLUGIN_REF:-main}"

# Every first-party plugin. Keep in step with plugins/ and registry/index.toml;
# scripts/ci.sh checks that this list and the directory agree.
ALL_PLUGINS="cargo-rust cmake-cpp doctor go make-generic node ports python-uv"

# The files a plugin is made of. Fixtures are deliberately not installed: they
# exist for plugin authors and CI, and kap never reads them at run time.
PLUGIN_FILES="plugin.kpl README.md"

say() { printf 'install-plugin: %s\n' "$1" >&2; }
die() { printf 'install-plugin: error: %s\n' "$1" >&2; exit 1; }

usage() {
    cat <<'USAGE'
usage: install-plugin.sh [--dest DIR] [--all] [--list] <plugin>...

  --dest DIR   where to install (default: $KAP_PLUGIN_DEST, else
               ~/.local/share/kap/plugins)
  --all        install every first-party plugin
  --list       print the available plugin names and exit

Environment:
  KAP_PLUGIN_DEST     same as --dest; set by kap when it runs this script
  KAP_PLUGIN_NAME     the plugin to install, when no name is given
  KAP_PLUGIN_SOURCE   raw content base URL (for a fork or a mirror)
  KAP_PLUGIN_REF      branch, tag, or commit (default: main)
USAGE
}

# --- download one URL to one path ------------------------------------------------
# curl's --fail matters: without it a 404 page is written to the output file and
# curl exits 0, and the caller cheerfully "installs" an HTML error page.
fetch() {
    url="$1"; out="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --proto '=https' -o "$out" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$out" "$url"
    else
        die "neither curl nor wget is installed"
    fi
}

dest="${KAP_PLUGIN_DEST:-}"
want_all=0
plugins=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dest) [ $# -ge 2 ] || die "--dest needs a directory"; dest="$2"; shift 2 ;;
        --dest=*) dest="${1#--dest=}"; shift ;;
        --all)  want_all=1; shift ;;
        --list) printf '%s\n' $ALL_PLUGINS; exit 0 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option '$1' (try --help)" ;;
        *) plugins="$plugins $1"; shift ;;
    esac
done

if [ "$want_all" -eq 1 ]; then
    plugins="$ALL_PLUGINS"
elif [ -z "${plugins# }" ] && [ -n "${KAP_PLUGIN_NAME:-}" ]; then
    plugins="$KAP_PLUGIN_NAME"
fi

[ -n "${plugins# }" ] || { usage >&2; die "no plugin named"; }

# --- where to put them ------------------------------------------------------------
# When kap runs this script it sets KAP_PLUGIN_DEST to a staging directory and
# expects the plugin's files *directly* there — it is installing one plugin and
# already knows its name. Run by hand, the natural target is the user plugin
# directory with one subdirectory per plugin.
if [ -n "${KAP_PLUGIN_DEST:-}" ] && [ "$dest" = "${KAP_PLUGIN_DEST:-}" ]; then
    flat=1
else
    flat=0
    if [ -z "$dest" ]; then
        dest="${XDG_DATA_HOME:-$HOME/.local/share}/kap/plugins"
    fi
fi

installed=0
for name in $plugins; do
    # Refuse a name that could escape the destination or the URL path. This
    # script may be invoked with a name that came off the network.
    case "$name" in
        */*|.*|"") die "'$name' is not a usable plugin name" ;;
    esac

    if [ "$flat" -eq 1 ]; then
        target="$dest"
    else
        target="$dest/$name"
    fi
    mkdir -p "$target"

    say "fetching $name"
    got_manifest=0
    for file in $PLUGIN_FILES; do
        url="$REPO_RAW/$REF/plugins/$name/$file"
        # Download beside the target and move into place, so an interrupted
        # download cannot leave a half-written plugin.kpl that kap would then
        # try to parse.
        if fetch "$url" "$target/$file.part" 2>/dev/null; then
            mv "$target/$file.part" "$target/$file"
            [ "$file" = "plugin.kpl" ] && got_manifest=1
        else
            rm -f "$target/$file.part"
            # Only plugin.kpl is required; a plugin without a README is odd but
            # not broken.
            [ "$file" = "plugin.kpl" ] && die "could not download $url"
        fi
    done

    [ "$got_manifest" -eq 1 ] || die "no plugin.kpl for '$name'"
    say "installed $name -> $target"
    installed=$((installed + 1))
done

say "$installed plugin(s) installed"

# A hint only when run by hand: when kap invoked us it is about to validate and
# report on its own, and a second voice would just be noise.
if [ "$flat" -eq 0 ]; then
    say "check them with: kap plugin doctor"
fi
