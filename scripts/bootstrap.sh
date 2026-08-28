#!/usr/bin/env bash
# scripts/bootstrap.sh
#
# First-time setup inside the dev container. Two modes:
#
#   kap-bootstrap --check-deps   (used during image build) — verify every tool
#                                the build needs is installed; exit non-zero
#                                if anything is missing so the image build
#                                fails loudly instead of half-working.
#
#   kap-bootstrap                (interactive) — configure + build + test once,
#                                then point you at the resulting binary.
#
# The Dockerfile copies this file to /usr/local/bin/kap-bootstrap *before* the
# repo is mounted, so the script must not depend on anything outside itself.
set -euo pipefail

check_deps()
{
    local missing=0
    for tool in cmake ninja c++ git make; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "kap: missing required tool: $tool" >&2
            missing=1
        fi
    done
    if [ "$missing" -ne 0 ]; then
        echo "kap: install the missing tools (see docker/dev.Dockerfile)." >&2
        exit 1
    fi
    echo "kap: all required tools present."
}

bootstrap()
{
    # When run from scripts/ the repo root is the parent; when `kap-bootstrap`
    # is invoked from /usr/local/bin inside the container, fall back to the
    # current directory (the WORKDIR /kap set by the Dockerfile).
    local repo_root
    repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." 2>/dev/null && pwd)"
    if [ ! -f "$repo_root/CMakeLists.txt" ]; then
        repo_root="$PWD"
    fi
    if [ ! -f "$repo_root/CMakeLists.txt" ]; then
        echo "kap: cannot find CMakeLists.txt; run kap-bootstrap from the repo root." >&2
        exit 1
    fi

    cd "$repo_root"
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    ctest --test-dir build --output-on-failure
    echo "kap: bootstrap complete. Try: ./build/kap --version"
}

case "${1:-}" in
    --check-deps) check_deps ;;
    *) bootstrap ;;
esac