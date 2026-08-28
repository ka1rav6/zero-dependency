#!/usr/bin/env bash
# docker/entrypoint.sh
#
# Runs *before* whatever command the user asked for inside the dev container
# (the ENTRYPOINT in dev.Dockerfile). It cheaply checks the container is wired
# up correctly and exports dev-only environment, then hands control to the
# requested command with `exec` so signals and exit codes flow straight
# through — a Ctrl-C in the shell kills the real tool, not a wrapper.
set -euo pipefail

# The dev workflow expects the live repo at /kap through the bind mount.
# Warn loudly (but do not fail) if it is missing, so a misconfigured `dev`
# shell is obvious instead of mysterious.
if [ ! -f /kap/CMakeLists.txt ]; then
    echo "kap: /kap does not look like this repository (did the volume mount?)" >&2
fi

# Tells kap's own code (later milestones) that it is running in a dev build.
export KAP_DEV="${KAP_DEV:-1}"

# Hand control to the user's command; default CMD is `bash`.
exec "$@"