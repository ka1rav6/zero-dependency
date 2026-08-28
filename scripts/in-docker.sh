#!/usr/bin/env bash
# scripts/in-docker.sh
#
# Run a command inside the dev container without remembering docker compose
# syntax. The repo is bind-mounted, so host-side edits are already visible in
# the container — this wrapper is just a shorthand for:
#
#     docker compose run --rm dev <cmd...>
#
# Examples:
#     ./scripts/in-docker.sh cmake --build build
#     ./scripts/in-docker.sh ctest --test-dir build
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# --rm discards the container after the command so quick commands never pile
# up. `exec` keeps the container's exit code as our own.
exec docker compose -f "$repo_root/docker-compose.yml" run --rm dev "$@"