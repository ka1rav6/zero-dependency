# kap

**Know project, act.** A zero-config CLI that works out what kind of project you are standing in and runs the right underlying tool.

```console
$ cd some-cmake-project && kap build      # cmake -S . -B build && cmake --build build
$ cd some-rust-crate    && kap test       # cargo test
$ cd some-monorepo      && kap dev        # every workspace at once, output labelled
```

Built in 72 hours for the **Zero Dependency** hackathon. **Track A — Developer Tools & CLI.**

---

## Two claims, both of them negative

Most projects are impressive because of what they contain. This one is impressive because of what it does not, and both absences are checkable in about thirty seconds.

### 1. The binary contains no third-party code

```console
$ ldd build/kap
	linux-vdso.so.1
	libstdc++.so.6
	libgcc_s.so.1
	libc.so.6
	libm.so.6
	/lib64/ld-linux-x86-64.so.2
```

libc, libstdc++, libm. That is the whole list, and it is the list the hackathon rules allow.

No toml++, no nlohmann/json, no CLI11, no Catch2, no GoogleTest, no OpenSSL, no fmt, no spdlog, no Boost. The TOML parser, the JSON parser, the SHA-256 implementation, the argument parser, the test harness, and a complete programming language with a lexer, parser, type checker, interpreter, and binary AST cache are all in `core/`, written this weekend, and each one is small enough to read in a sitting.

**15,397 lines** of C++20 in `core/`. No `vendor/`, no `third_party/`, no submodules.

### 2. The binary contains no ecosystem knowledge

`kap build` in a Rust crate runs `cargo build` because a 60-line text file says so — not because the binary has a branch for Rust. Detection rules, command recipes, and configuration schemas all live in **KPL plugins**, a small language the core interprets.

The word "cargo" does appear in `core/` — fifteen times, every one of them a comment or a line of `kap help` prose. None of them is a code path. The honest proof is not a grep, it is this:

```console
$ mv ~/.local/share/kap/plugins /tmp/elsewhere
$ kap build
error: no plugin claims this directory
       note: no plugins found. kap looked in: ...

$ mv /tmp/elsewhere ~/.local/share/kap/plugins
$ kap build                                        # works again
```

Take the text files away and kap can do nothing at all. That is not a layering nicety — it is the difference between a tool that supports six ecosystems and a tool that supports any ecosystem someone will spend two minutes describing.

That is not a layering nicety. It is the difference between a tool that supports six ecosystems and a tool that supports any ecosystem someone is willing to spend two minutes describing.

---

## Try it in ninety seconds

Everything below runs **without cmake, cargo, npm, Go, Java, or Zig installed.** That is not a demo trick — `kap build -n` is complete rather than best-effort, because plugins only *declare* steps and the executor is the only thing in the codebase that can start a process.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local
```

```console
$ kap detect                  # who owns this directory, and why
cmake-cpp  priority=30 score=1
  markers: CMakeLists.txt
  source: bundled (/home/you/.local/share/kap/plugins/cmake-cpp)

$ kap build -n                # exactly what would run, without running it
  kap build · cmake-cpp · 3 steps   — nothing will run

  1 $ mkdir -p build
  2 $ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
  3 $ cmake --build build

$ kap doctor                  # do I have the tools this project needs
  cmake-cpp, doctor, ports

  ok  cmake               required
  ok  ninja               optional
  --  ccache              optional  not installed

  healthy    every required tool is present
```

For a **single self-contained binary** that carries all ten plugins and needs no files beside it and no network:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKAP_EMBED_PLUGINS=ON
```

---

## The plugin system

This is the part worth your attention.

### A plugin is one text file

```kpl
manifest { name = "zig" version = "1.0.0" api_version = 1 priority = 42 }

detect   { file_exists "build.zig" }
requires { any_of [zig] }

schema {
  optimize: enum { debug, release_safe, release_fast } = debug
  test_step: str = "test"
}

command build(project, config, extra) {
  let cmd = ["zig", "build"]
  let optimize = match config.optimize {
    debug        => none,
    release_safe => "ReleaseSafe",
    release_fast => "ReleaseFast",
  }
  if optimize != none { cmd = cmd + ["-Doptimize=" + optimize] }
  step cmd + extra
}
```

Five blocks. `manifest` is identity and ranking, `detect` is which directories this claims, `requires` is what `kap doctor` reports on, `schema` declares every key a user may override, and `command` blocks say what each kap verb runs.

### KPL is a real language, not string templating

It has a lexer, a recursive-descent parser, a **type checker that runs at load time**, a tree-walking interpreter, and a binary AST cache so the hot path never re-parses text.

```console
$ kap plugin doctor bad
[FAIL] bad
kap: error: bad/plugin.kpl:
  condition must be a boolean, got string at line 4:41
```

That error arrives *before* anything runs, located to the line and column. A misconfigured plugin is caught by the checker, not by a build failing halfway through.

The language has: `let` bindings and assignment, `if`/`else if`/`else`, `for` over lists, exhaustively-checked `match`, inline conditionals, integers, strings, booleans, lists, records, a `none` literal, and seven host builtins. It deliberately has **no user-defined functions and no recursion** — a plugin is a description, and an interpreter small enough to audit is worth more than one that is expressive.

### Plugins cannot do anything dangerous

A plugin cannot spawn a process, read outside the project root, reach the network, see another plugin, or observe the clock or a random number.

| | |
|---|---|
| Process execution | None. `step` appends to a list; only kap's executor spawns |
| Filesystem | `exists`, `read`, `glob` only — paths canonicalised, escapes refused |
| Read size | 1 MiB per read; 10,000 glob results |
| Environment | Deny-listed: `*_TOKEN`, `*_KEY`, `*_SECRET`, `*_PASSWORD`, `AWS_*` |
| Network | None |
| Randomness, time | None — the same inputs always produce the same steps |

That last row is why `--dry-run` is **complete rather than best-effort**, and why plugins can be tested without installing a single toolchain:

```console
$ kap plugin test
74 cases: 74 passed, 0 failed
```

Each case evaluates a command against a fixture project tree and diffs the resulting step list against a committed golden file. CI verifies all ten plugins across every ecosystem without cmake, cargo, npm, Go, Java, or Zig anywhere on the machine.

### Detection is declarative too

Several plugins can match one directory. Ranking is by `priority`; composable "sidecars" ride alongside the winner rather than competing with it; and a genuine tie is **refused rather than guessed**:

```console
kap: error: cannot tell which plugin owns /home/you/code/hybrid
      note: these plugins matched at the same priority (30): alpha, beta
      note: pin one in kap.toml:
      note:     [detect]
      note:     ecosystem = "alpha"
```

When nothing claims a directory, kap shows its work rather than shrugging:

```console
$ kap detect
error: no plugin claims this directory
       /home/you/code/backend

  closest matches, none fired
    go          ✗ go.mod   ✗ go.work
    zig         ✗ build.zig   ✗ build.zig.zon
    cargo-rust  ✗ Cargo.toml

  also available   doctor, ports   sidecars — they claim every directory
  considered       10 plugins      --verbose to list them

  help: each marked file above is one kap looks for. Creating the one that
        fits this project lets that plugin claim it.
```

### Write one yourself

```console
$ kap plugin new elixir          # scaffolds the five blocks and a fixture test
$ kap plugin doctor elixir       # parses, validates, type-checks
$ kap plugin test elixir         # runs its golden cases — no Elixir needed
$ kap plugin install --link elixir
```

`--link` symlinks instead of copying, so edits take effect immediately.

---

## Bundled plugins

| Plugin | Claims | Notable |
|---|---|---|
| [`cmake-cpp`](kap-plugins/cmake-cpp/README.md) | `CMakeLists.txt` | Never contradicts an existing `CMakeCache.txt` generator |
| [`cargo-rust`](kap-plugins/cargo-rust/README.md) | `Cargo.toml` | `kap ci` is fmt-check, clippy, test |
| [`node`](kap-plugins/node/README.md) | `package.json` | Reads the lockfile for npm/pnpm/yarn/bun; workspace-aware `dev` |
| [`go`](kap-plugins/go/README.md) | `go.mod`, `go.work` | `-race` on by default; golangci-lint when present |
| [`python-uv`](kap-plugins/python-uv/README.md) | `uv.lock`, `pyproject.toml` | Every command is `uv run`, so there is no venv to activate |
| [`java`](kap-plugins/java/README.md) | `pom.xml`, `build.gradle`(`.kts`) | Prefers `./mvnw` and `./gradlew` over the system tool |
| [`zig`](kap-plugins/zig/README.md) | `build.zig`, `build.zig.zon` | Reads `build.zig` to see which steps exist before running one |
| [`make-generic`](kap-plugins/make-generic/README.md) | `Makefile` | The fallback; every target is configurable |
| [`doctor`](kap-plugins/doctor/README.md) | everything | Reports on the tools other plugins require |
| [`ports`](kap-plugins/ports/README.md) | everything | `ss` / `lsof` / `netstat`, whichever you have |

**`doctor` and `ports` are themselves written in KPL, not C++.** The design claims the language reaches system introspection; those two plugins are the proof, and they are why `kap doctor` works in a directory no ecosystem plugin claims.

---

## Errors are a feature

A tool that runs other tools inherits their error messages, and those messages never know why *you* were invoked. kap fixes that where it can.

A plugin can look at your project and refuse before spawning anything:

```console
$ kap run
error: package.json declares no "start" script, which is what 'kap run' runs

         this project has a "dev" script
           kap dev
```

npm can only say `Missing script: "start"`. It cannot know that `kap run` chose the name, that `start_script` in `kap.toml` changes it, or that a sibling command already does what you wanted.

---

## Documentation

Everything in `docs/`, and it is worth a look — the design is documented as thoroughly as it is implemented.

| | |
|---|---|
| **[docs/README.md](docs/README.md)** | Index — start here |
| **[docs/usage.md](docs/usage.md)** | The guided tour: installing, the first five minutes, every workflow |
| **[docs/commands.md](docs/commands.md)** | Exhaustive reference — every command, flag, exit code, and file |
| **[docs/configuration.md](docs/configuration.md)** | `kap.toml`, the five config layers, hooks |
| **[docs/plugins.md](docs/plugins.md)** | Writing, testing, installing, and publishing a plugin |
| **[docs/PLUGIN_API.md](docs/PLUGIN_API.md)** | The KPL language reference, with a full EBNF grammar |
| **[docs/design.md](docs/design.md)** | The design document: every decision and why |
| **[docs/dockerusage.md](docs/dockerusage.md)** | The pinned dev container |
| **[howto.md](howto.md)** | Contributor guide: what every module does |
| **[AGENTS.md](AGENTS.md)** | The rules every change in this repo follows |

---

## Testing

```console
$ ./scripts/ci.sh
ci.sh: all green
```

| | |
|---|---|
| **412** unit tests | in-tree harness (`tests/harness.hpp`), no Catch2 or GoogleTest |
| **256** end-to-end tests | the real binary, driven by a shell suite |
| **74** plugin golden cases | every plugin, no ecosystem toolchains required |

`ci.sh` configures and builds with `-Werror`, runs all three suites, enforces `clang-format`, type-checks every plugin, dogfoods detection on kap's own repository, installs to a scratch prefix and runs the result **with an empty environment**, builds the embedded variant and proves it works on a bare machine, and syntax-checks the generated shell completions and the install script.

---

## Layout

```
core/                C++ source — the binary and the library it links
  cli, config          command line, layered configuration
  detect               which plugin owns a directory
  kpl, kapc            the plugin language: lexer, parser, checker, interpreter, AST cache
  exec                 the only place in kap that creates a process
  registry             the plugin manager: index, lockfile, install pipeline
  toml, json, sha256   in-tree parsers and hashing, because the rules permit no libraries
  style                terminal colour, with NO_COLOR and non-UTF-8 fallbacks
kap-plugins/         the ten first-party plugins, each with fixture tests
registry/index.toml  the plugin registry
tests/               unit tests (in-tree harness) and an end-to-end shell suite
docker/, scripts/    the pinned dev container, CI, and the install script
docs/                everything above, documented
```

---

## License

MIT. See [LICENSE](LICENSE).
