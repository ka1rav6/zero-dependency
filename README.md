# kap

**kap** = "know project, act". A zero-config CLI that works out what kind of
project you are standing in and runs the right underlying tool.

```console
$ cd some-cmake-project && kap build      # cmake -S . -B build && cmake --build build
$ cd some-rust-crate    && kap test       # cargo test
$ cd some-monorepo      && kap dev        # every workspace at once, output labelled
```

Two ideas hold it together.

**The binary knows nothing about any ecosystem.** It does not contain the word
"cargo". Detection rules, command recipes, and configuration schemas all live in
**KPL plugins** — one text file per ecosystem, interpreted by the core. Adding
support for a new build system means writing a `.kpl` file, not rebuilding kap.

**Zero dependencies.** kap links against the C++20 standard library and POSIX,
and nothing else: no toml++, no nlohmann/json, no CLI11, no Catch2, no OpenSSL.
The TOML parser, the JSON parser, the argument parser, the test harness, the
SHA-256 implementation, and the plugin language are all in-tree, and each one is
small enough to read. That is the project, not a constraint it works around.

Built for the zero-dependency hackathon.

---

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/ka1rav6/zero-dependency/main/scripts/install.sh | sh
```

Clones, builds, **runs the test suite**, and installs the binary together with
eight plugins and the registry index. A build that fails its own tests is not
installed. Needs `git`, `cmake`, and a C++20 compiler.

The plugins land twice: under `<prefix>/share/kap/` for the binary the script
just installed, and under `~/.local/share/kap/plugins`, which **every** kap on
the machine reads. A `kap` you built in a checkout afterwards sees them too.

For a **single self-contained binary** — plugins compiled in, works with
nothing beside it and no network:

```sh
curl -fsSL .../install.sh | KAP_EMBED=1 sh
```

From source:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix ~/.local

# ...and, to make them global rather than tied to that one prefix:
cp -r kap-plugins/*/ ~/.local/share/kap/plugins/

# ...or self-contained
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKAP_EMBED_PLUGINS=ON
```

---

## Five commands worth knowing

```sh
kap help          # every command, each with its own page
kap detect        # which plugin claims this directory, and which files made it match
kap doctor        # are the tools this project needs actually installed
kap build -n      # exactly what `kap build` would run, without running it
kap build         # run it
kap plugin list   # what is installed, where it came from, whether it is enabled
```

`kap build -n` is the habit worth forming. kap never runs anything it will not
show you first.

The full surface is twelve commands — `build`, `check`, `ci`, `clean`, `dev`,
`doctor`, `fmt`, `install`, `lint`, `ports`, `run`, `test` — plus `detect`,
`config`, `plugin`, and `completions`. See **[docs/usage.md](docs/usage.md)**.

---

## Bundled plugins

| Plugin | Claims | Notable |
|---|---|---|
| [`cmake-cpp`](kap-plugins/cmake-cpp/README.md) | `CMakeLists.txt` | Picks Ninja when it is installed |
| [`cargo-rust`](kap-plugins/cargo-rust/README.md) | `Cargo.toml` | `kap ci` is fmt-check, clippy, test |
| [`node`](kap-plugins/node/README.md) | `package.json` | Reads the lockfile for npm/pnpm/yarn/bun; workspace-aware `dev` |
| [`go`](kap-plugins/go/README.md) | `go.mod`, `go.work` | `-race` on by default; golangci-lint when present |
| [`python-uv`](kap-plugins/python-uv/README.md) | `uv.lock`, `pyproject.toml` | Every command is `uv run`, so no venv to activate |
| [`make-generic`](kap-plugins/make-generic/README.md) | `Makefile` | The fallback; every target is configurable |
| [`doctor`](kap-plugins/doctor/README.md) | everything | Reports on the tools other plugins require |
| [`ports`](kap-plugins/ports/README.md) | everything | `ss` / `lsof` / `netstat`, whichever you have |

`doctor` and `ports` are themselves written in KPL, not C++ — the design says
the language has to reach system introspection, and that is the proof.

---

## Write your own

```console
$ kap plugin new zig
$ kap plugin doctor zig          # parses, validates, type-checks
$ kap plugin test zig            # runs its fixture cases
$ kap plugin install --link zig  # a symlink: edits take effect immediately
```

A plugin is one file:

```kpl
manifest { name = "zig" version = "1.0.0" api_version = 1 priority = 35 }
detect   { file_exists "build.zig" }
requires { any_of [zig] }
schema   { release: bool = false }

command build(project, config, extra) {
  let flags = if config.release then ["-Doptimize=ReleaseSafe"] else []
  step ["zig", "build"] + flags + extra
}
```

Plugins cannot spawn processes, read outside the project root, reach the
network, or see each other. They declare *what should run*; kap's executor is
the only thing that runs it — which is what makes `--dry-run` complete rather
than best-effort.

**[docs/plugins.md](docs/plugins.md)** is the guide;
**[docs/PLUGIN_API.md](docs/PLUGIN_API.md)** is the language reference.

---

## Documentation

| | |
|---|---|
| [docs/README.md](docs/README.md) | Index |
| [docs/usage.md](docs/usage.md) | The guided tour |
| [docs/commands.md](docs/commands.md) | Exhaustive reference: every command, flag, and exit code |
| [docs/configuration.md](docs/configuration.md) | `kap.toml`, the layers, hooks |
| [docs/plugins.md](docs/plugins.md) | Writing, testing, installing, publishing a plugin |
| [docs/PLUGIN_API.md](docs/PLUGIN_API.md) | KPL reference and grammar |
| [docs/dockerusage.md](docs/dockerusage.md) | The pinned dev container |
| [docs/design.md](docs/design.md) | The design document and roadmap |
| [howto.md](howto.md) | Contributor guide: how the code works |
| [AGENTS.md](AGENTS.md) | The rules every change follows |

---

## Layout

```
core/                C++ source — the binary and the library it links
  cli, config          command line, layered configuration
  detect               which plugin owns a directory
  kpl, kapc            the plugin language: lexer, parser, checker, interpreter, AST cache
  exec                 the only place in kap that creates a process
  registry             the plugin manager: index, lockfile, install pipeline
  toml, json, sha256   in-tree parsers and hashing, because §9 permits no libraries
kap-plugins/         the eight first-party plugins, each with fixture tests
registry/index.toml  the plugin registry
tests/               unit tests (in-tree harness) and an end-to-end shell suite
docker/, scripts/    the pinned dev container, CI, and the install script
docs/                everything above
```

---

## Developing

```sh
docker compose run --rm dev ./scripts/ci.sh    # the pinned container CI uses
./scripts/ci.sh                                # or on the host
```

`ci.sh` configures, builds with `-Werror`, runs the unit tests and the
end-to-end suite, enforces `clang-format`, type-checks every plugin, runs every
plugin's fixture cases, dogfoods detection on kap's own repository, installs to
a scratch prefix and runs the result with an empty environment, and syntax-checks
the generated completion scripts and the install script.

```sh
docker compose --profile full run --rm dev-full ./scripts/ci-full.sh
```

`ci-full.sh` closes the gap the fast suite cannot: it creates a real project of
each of the six kinds and runs kap against real toolchains. The ordinary suite
deliberately needs none of them.

Sanitizers, when touching a parser:

```sh
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKAP_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

---

## Status

**v1.0.0** — all eleven milestones in [docs/design.md](docs/design.md) §11 are
complete: the dev container and CI, the infrastructure libraries, the KPL
front-end and interpreter, the detection engine, the executor, the config merge
and CLI wiring, the plugin manager, six ecosystem plugins, the two system
plugins, and the polish.

384 unit tests, 198 end-to-end assertions, 52 plugin fixture cases, and 17
ecosystem checks against real toolchains.

## Licence

[MIT](LICENSE).
