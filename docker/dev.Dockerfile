# docker/dev.Dockerfile
#
# The fat developer image. Per design doc §10.5 we pin the *digest*, not just
# the ubuntu:24.04 tag, because tags are mutable while digests are immutable —
# that is what guarantees every contributor and CI byte-for-byte toolchain
# parity.
#
# This image builds and tests kap itself. Ecosystem tools that *kap's plugins*
# invoke at runtime (cargo, npm, go, ...) deliberately live elsewhere: the
# optional dev-full image (roadmap Milestone 10) and in plugins, never here.

FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517

# --no-install-recommends keeps out docs/locales we never read; deleting the
# apt lists afterwards stops the layer from ballooning.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    clang-format \
    # python3 is NOT a build dependency of kap; it is installed so the
    # dogfooded first-party plugins (Milestone 8+) can run inside the
    # container instead of only on the host.
    python3 \
    # curl and python3 together are what tests/e2e.sh needs to serve a plugin
    # installer over loopback and install from it. Without them that whole
    # section silently SKIPs, so the installer-script path — the one place kap
    # runs downloaded code — would be untested in CI and only ever exercised
    # on a developer's laptop.
    curl \
    # The `ports` plugin declares `any_of [ss, lsof, netstat]`, and `doctor`
    # fails when a matched plugin's requirement cannot be met. Both are
    # sidecars that claim *every* directory, so without one of these three the
    # suite's "doctor still works in a directory nothing owns" case fails here
    # and passes on any normal Linux host — exactly the kind of divergence the
    # pinned image exists to prevent. iproute2 provides `ss`.
    iproute2 \
    lsof \
    net-tools \
    && rm -rf /var/lib/apt/lists/*

# The bootstrap script is copied in *before* the repo is bind-mounted, so the
# image build itself can self-check the toolchain via `--check-deps`.
COPY scripts/bootstrap.sh /usr/local/bin/kap-bootstrap
RUN kap-bootstrap --check-deps

# The entrypoint is copied into the image so the image also works without a
# bind mount; at dev time the compose volume overlays it with the repo's own
# copy (identical file, so the behaviour cannot drift).
COPY docker/entrypoint.sh /kap/docker/entrypoint.sh
ENTRYPOINT ["/kap/docker/entrypoint.sh"]

WORKDIR /kap
CMD ["bash"]