# Dockerfile
#
# Multi-stage "release-ish" image (design doc §10.1): stage 1 builds kap
# inside a full toolchain; stage 2 keeps only what is needed to *run* the
# binary, shrinking the delivered image dramatically.
#
# This is NOT the daily-driver images — that is docker/dev.Dockerfile. Use
# this one when you want to run `kap` as a container command:
#
#     docker build -t kap . && docker run --rm kap --version
#
# The base-image digest is pinned (same reason as docker/dev.Dockerfile): tags
# drift, digests do not.

FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# .dockerignore keeps build/ and .git out of the context, so this COPY is
# just the real source.
COPY . /src

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build

FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517 AS runtime

# ca-certificates so later milestones can reach the plugin registry.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/kap /usr/local/bin/kap
ENTRYPOINT ["/usr/local/bin/kap"]