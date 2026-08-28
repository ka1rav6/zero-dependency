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
plugins/             # first-party KPL plugins (+ their fixture tests)
registry/            # plugin index
docker/              # dev container + entrypoint
scripts/             # in-docker wrapper, bootstrap, CI
tests/               # zero-dep unit tests (in-tree harness)
docs/design.md       # design doc & incremental roadmap
howto.md             # contributor guide: how the code works, how to add to it
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

## Contributing

`howto.md` is the contributor guide: what every module does, how the test
harness works, how to add a feature end to end, and the pitfalls worth knowing
about before your first commit. `AGENTS.md` has the non-negotiable rules.

Run the full pipeline before you commit — if this is green, CI will be:

```sh
./scripts/ci.sh
```

### Status

Milestones 0 through 3 are complete: the dev container and CI, the shared
infrastructure libraries (diagnostics, sandboxed filesystem, TOML, JSON, CLI),
the whole KPL front-end and interpreter, the `.kapc` AST cache, and
`kap plugin doctor` / `kap plugin test`.

```sh
kap plugin doctor        # parse, validate, and type-check every plugin
kap plugin test          # run each plugin's fixture cases
```

`kap plugin test` evaluates a plugin's command blocks against fixture project
trees and compares the resulting step lists with committed golden files —
without executing a single real build tool, so CI needs no ecosystem
toolchains.

Next up is Milestone 4, the detection engine. See `docs/design.md` §11 for the
full roadmap.