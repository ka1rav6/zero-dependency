# docker/dev-full.Dockerfile
#
# The `dev-full` image (design doc §10.5's optional tag, Milestone 10).
#
# `dev` builds and tests kap itself. This image adds the *ecosystem toolchains*
# the first-party plugins drive — Rust, Node, Go, Python/uv — so that
# `kap build` can be exercised against real projects of each kind rather than
# only against fixtures.
#
# ## Why this is a separate image
#
# It is roughly ten times the size of `dev` and rebuilds far more often, since
# it tracks four upstream release cadences instead of one. Making it the default
# would mean every contributor waiting on a Rust toolchain download to fix a
# typo in the TOML parser. So `dev` stays the fast path, and this is what you
# reach for when you are changing a plugin.
#
# ## Build and use
#
#     docker compose --profile full build dev-full
#     docker compose --profile full run --rm dev-full
#
#     # inside:
#     ./scripts/ci.sh                 # the same CI kap runs everywhere
#     ./scripts/ci-full.sh            # plus real builds in each ecosystem
#
# Note that kap's *unit* tests and *plugin fixture* tests need none of this —
# that is the point of evaluating command blocks without executing them (§5.2).
# This image exists for the last mile: proving the argv arrays kap produces are
# ones the real tools accept.

FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517

ENV DEBIAN_FRONTEND=noninteractive

# --- kap's own toolchain, identical to dev.Dockerfile ---------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    clang-format \
    clang-tidy \
    curl \
    xz-utils \
    # `ports` reaches for one of these three; iproute2 provides `ss`.
    iproute2 \
    lsof \
    net-tools \
    && rm -rf /var/lib/apt/lists/*

# --- Go -------------------------------------------------------------------------
# From upstream rather than apt: Ubuntu's golang package lags by a release or
# more, and a plugin that emits `go test -race` should be tested against the Go
# people actually have.
ARG GO_VERSION=1.23.4
RUN curl -fsSL "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" \
      -o /tmp/go.tar.gz \
 && tar -C /usr/local -xzf /tmp/go.tar.gz \
 && rm /tmp/go.tar.gz
ENV PATH="/usr/local/go/bin:${PATH}" \
    GOPATH="/root/go" \
    GOTOOLCHAIN=local

# --- Rust -----------------------------------------------------------------------
# rustup with the components the cargo-rust plugin's lint and fmt commands need.
ARG RUST_VERSION=1.83.0
RUN curl -fsSL https://sh.rustup.rs -o /tmp/rustup.sh \
 && sh /tmp/rustup.sh -y --no-modify-path --profile minimal \
      --default-toolchain "${RUST_VERSION}" \
      --component rustfmt --component clippy \
 && rm /tmp/rustup.sh
ENV PATH="/root/.cargo/bin:${PATH}"

# --- Node -----------------------------------------------------------------------
# NodeSource for a current LTS. corepack ships with Node and provides pnpm and
# yarn shims, which is exactly what the node plugin's package-manager detection
# needs to be exercised against.
ARG NODE_MAJOR=22
RUN curl -fsSL "https://deb.nodesource.com/setup_${NODE_MAJOR}.x" | bash - \
 && apt-get install -y --no-install-recommends nodejs \
 && rm -rf /var/lib/apt/lists/* \
 && corepack enable

# --- Python + uv ------------------------------------------------------------------
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-venv \
 && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://astral.sh/uv/install.sh -o /tmp/uv.sh \
 && sh /tmp/uv.sh \
 && rm /tmp/uv.sh
ENV PATH="/root/.local/bin:${PATH}"

# --- Same bootstrap contract as the dev image --------------------------------------
COPY scripts/bootstrap.sh /usr/local/bin/kap-bootstrap
RUN kap-bootstrap --check-deps

COPY docker/entrypoint.sh /kap/docker/entrypoint.sh
ENTRYPOINT ["/kap/docker/entrypoint.sh"]

WORKDIR /kap
CMD ["bash"]
