# kap — zero-dependency

Our submission for the zero-dependency hackathon: a zero-config CLI that
detects what kind of project you're standing in and runs the right underlying
tool for common tasks (`build`, `test`, `lint`, `run`, ...).

- **Zero external dependencies** — C++20 stdlib + POSIX APIs only.
- Per-ecosystem knowledge lives in **KPL plugins** (a small DSL), not in the
  binary. See `docs/design.md` for the full design doc and roadmap.

## Layout

```
CMakeLists.txt       # build system (CMake + Ninja)
core/                # C++ source (the kap binary + library)
plugins/             # first-party KPL plugins
registry/            # plugin index
docker/              # dev container + entrypoint
scripts/             # in-docker wrapper, bootstrap, CI
tests/               # zero-dep unit tests (in-tree harness)
docs/design.md       # design doc & incremental roadmap
```

## Build & test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/kap --version

# run the unit tests
ctest --test-dir build --output-on-failure
```

## Dev environment (Docker)

```sh
docker compose run --rm dev
# inside the container:
./scripts/ci.sh
```

See `docs/dockerusage.md` for a full Docker guide.