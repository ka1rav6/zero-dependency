# kap — Design Document & Roadmap

**kap** = "know project, act". A zero-config CLI that detects what kind of
project you're standing in and runs the right underlying tool for common
tasks (`build`, `test`, `lint`, `run`, ...). Per-ecosystem knowledge lives
in **KPL plugins** (Kap Plugin Language — a purpose-built DSL interpreted
by the C++ core), not in hardcoded logic inside the binary.

This is a from-scratch, C++ redesign of the `uu` idea (Rust core +
hardcoded per-language modules). The core difference: `kap`'s binary knows
nothing about any specific ecosystem. It only knows how to detect, dispatch,
parse KPL, and execute. Everything about "what command builds a CMake
project" lives in a `.kpl` file that ships as a plugin.

---

## 1. Goals & Non-Goals

### Goals
- A small, fast **C++20 core binary** built with **zero third-party
  dependencies** — only the C++ standard library and POSIX APIs (see
  §12). No rebuild when a new ecosystem or build-tool variant appears.
- **Project detection** that's rule-based, prioritized, and plugin-owned
  (declared in each plugin's `detect` block).
- A **Kap Plugin Language (KPL)** — a small, sandboxed DSL that is the
  *only* extension surface. Plugin authors never write C++ or embed a
  general-purpose scripting runtime; KPL contains everything needed to
  declare detection rules, config schemas, and command recipes.
- **Layered customization**: plugin defaults → user global config →
  per-project config → CLI flags, so a user can override the CMake
  generator, choose `ninja` over `make`, add pre/post hooks, etc.,
  without forking the plugin.
- A **plugin manager** (`kap plugin ...`) that installs **KPL plugins**
  — one per build system / ecosystem (`cmake-cpp`, `cargo-rust`, `meson`,
  `node`, ...) — from a registry, a git URL, or a local path.
- **Synchronized developer environment** via Docker: every contributor
  builds and tests `kap` inside the same container image so toolchain
  drift cannot happen (see §13).
- Safe-by-default KPL sandboxing: plugins construct command argv arrays
  and read project metadata through host-provided builtins; they cannot
  spawn processes, open arbitrary files, or reach the network.

### Non-Goals
- `kap` is not a build system. It never parses source files, resolves
  dependency graphs, or caches build artifacts — it shells out to the
  real tool (`cargo`, `cmake`, `go`, `npm`, ...).
- `kap` does not try to unify build *semantics* across ecosystems, only
  the **command surface** ("what do I type to build this").
- No GUI. CLI only for v1.
- No embedded Lua, Python, JavaScript, or any other general-purpose
  language. KPL is intentionally limited.
- No third-party C/C++ libraries in the `kap` binary itself (no sol2,
  no CLI11, no toml++, no Catch2). If we need TOML or a CLI parser, we
  write a minimal in-tree implementation.

---

## 2. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         kap (C++20 core)                          │
│                                                                   │
│  ┌───────────────┐   ┌────────────────┐   ┌──────────────────┐ │
│  │  CLI parser    │──▶│ Detection engine│──▶│  Plugin loader   │ │
│  │ (kap build ...)│   │ (walks cwd/tree)│   │  (finds match)   │ │
│  └───────────────┘   └────────────────┘   └────────┬─────────┘ │
│                                                       │           │
│                                                       ▼           │
│                                            ┌────────────────────┐ │
│                                            │  KPL interpreter   │ │
│                                            │  (parse + eval     │ │
│                                            │   plugin.kpl)      │ │
│                                            └─────────┬──────────┘ │
│                                                       │           │
│                                                       ▼           │
│                                            ┌────────────────────┐ │
│                                            │ Command executor   │ │
│                                            │ (posix_spawn,      │ │
│                                            │  dry-run, env)     │ │
│                                            └────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
         ▲                                          ▲
         │                                          │
   ~/.config/kap/config.toml                 ~/.local/share/kap/plugins/*
   ./kap.toml (project overrides)            ./.kap/plugins/*  (project-local)
```

Four subsystems, deliberately decoupled:

1. **Detection engine** — pure C++, no KPL involved at eval time (detect
   rules are *declared* in KPL but compiled to a static rule table at
   plugin load). Fast filesystem scan, returns a ranked list of candidate
   ecosystems.
2. **Plugin loader** — resolves which installed plugin(s) match, applies
   config-driven overrides, parses `plugin.kpl` once (cached bytecode/AST
   on disk).
3. **KPL interpreter** — evaluates a `command` block in a fresh sandboxed
   environment per invocation. Plugins are pure data/logic, no persistent
   state across runs.
4. **Executor** — takes the plugin's returned `CommandSpec` and actually
   runs it via `posix_spawn`/`execve`, handling dry-run, cwd, env
   injection, and output streaming (needed for `kap dev` concurrent
   workspace commands).

---

## 3. Project Detection

### 3.1 Design principle
Detection is **declarative and plugin-owned**, not hardcoded. Each plugin
ships a `detect` block in `plugin.kpl`. The core's detection engine
evaluates those declarations — it has zero built-in knowledge of
"Cargo.toml means Rust."

### 3.2 Detection algorithm

```
1. Determine the search root:
     - default: current working directory
     - `--root <path>` overrides it
     - walk upward (like git does for .git) up to N levels (config:
       detect.max_walk_up, default 0 — i.e. cwd only, opt-in upward walk)

2. For every enabled plugin, evaluate its compiled `detect` rules against
   the search root:
     - file_exists: "Cargo.toml"
     - file_exists_any: ["*.xcworkspace", "*.xcodeproj"]
     - file_contains: { path: "package.json", pattern: "\"workspaces\"" }
     - dir_exists: ".git/modules"
   Each satisfied rule contributes a match score.

3. Each plugin declares a numeric `priority` AND may declare
   `supersedes: ["gradle-generic"]` to explicitly say "if I match, ignore
   this other plugin."

4. Collect all plugins whose rules matched. Remove any plugin listed in
   another matched plugin's `supersedes`. If more than one remains:
     - if project config (`kap.toml`) pins `detect.ecosystem = "node"`,
       use that and stop.
     - else pick the highest `priority`.
     - if still tied, error out listing the tie and ask the user to pin
       one in `kap.toml` (never silently guess on a real tie).

5. Cache the resolution in `.kap/cache.json` (gitignored) keyed by a hash
   of the matched marker files' mtimes.
```

### 3.3 Monorepo / multi-plugin detection
- A plugin can declare `composable: true`, meaning it runs *alongside*
  another matched plugin (e.g. a `docker-compose` sidecar that adds
  `kap up`/`kap down` without claiming `build`/`test`).
- For workspace-style tools, the matched plugin returns multiple
  concurrent step specs; the executor runs them with prefixed output.

### 3.4 What the core exposes to KPL at command time
A read-only `project` value passed into every `command` block:

```kpl
// Available as `project` inside command blocks:
project.root            // string — absolute path
project.matched_files   // list<string> — which markers fired
project.exists(path)    // bool — relative to root
project.read(path)      // string — capped at 1 MiB, sandboxed to root
project.glob(pattern)   // list<string> — relative paths, sandboxed
project.env(name)       // string? — filtered environment lookup
project.tool(name)      // bool — checks PATH without exec
```

---

## 4. Command Lifecycle (what happens on `kap build`)

```
kap build -- --release
   │
   ├─ 1. CLI parse: command="build", passthrough_args=["--release"], flags={}
   ├─ 2. Load config: merge (defaults, ~/.config/kap/config.toml, ./kap.toml, CLI flags)
   ├─ 3. Detection engine resolves ecosystem -> plugin "cmake-cpp"
   ├─ 4. Plugin loader loads cmake-cpp's AST (from cache or re-parse plugin.kpl)
   ├─ 5. Core evaluates: commands.build(project, config, extra_args)
   │       -> returns a CommandSpec (see §5.4)
   ├─ 6. Executor:
   │       - if --dry-run: print the resolved shell command(s), exit 0
   │       - else: run in project.root (or spec.cwd), stream stdout/stderr,
   │         propagate exit code
   └─ 7. Exit with the underlying tool's exit code (never swallow failures)
```

Every abstract command (`build`, `check`, `ci`, `clean`, `dev`, `doctor`,
`fmt`, `install`, `lint`, `ports`, `run`, `test`) follows this shape.
`ports` and `doctor` are implemented as **bundled KPL plugins**, not
hardcoded C++ — proving the DSL is expressive enough for system
introspection, not just "shell out to a build tool."

---

## 5. Kap Plugin Language (KPL)

KPL is the single extension surface for `kap`. A plugin is one (or more)
`.kpl` files plus a `README.md`. There is no Lua, no WASM, no dynamic
library loading of plugin code — only text that the core parses and
interprets.

### 5.1 Design goals for KPL

| Goal | How KPL achieves it |
|---|---|
| Readable without learning a full language | Rust-like blocks, obvious keywords, minimal punctuation |
| Sandboxed | No `exec`, `import`, `ffi`, or raw filesystem APIs — only `project.*` builtins |
| Zero-deps in the core | Hand-written recursive-descent parser + tree-walk interpreter (~2–3k LOC) |
| Testable | `kap plugin test` evaluates command blocks against fixtures; asserts on returned steps |
| Diff-friendly | One `plugin.kpl` per ecosystem; manifest + detect + schema + commands in one file |
| Deterministic | No threads, no randomness, no network; same inputs → same `CommandSpec` |

### 5.2 Plugin package layout

```
cmake-cpp/
├── plugin.kpl         # manifest + detect + schema + commands (the whole plugin)
└── README.md
```

Optional test fixtures (not loaded at runtime):

```
cmake-cpp/
├── plugin.kpl
├── tests/
│   ├── fixtures/simple-project/CMakeLists.txt
│   └── expected/build.steps.json
└── README.md
```

### 5.3 Complete `plugin.kpl` example — `cmake-cpp`

```kpl
manifest {
  name        = "cmake-cpp"
  version     = "1.2.0"
  api_version = 1
  priority    = 30
  composable  = false
  supersedes  = []
}

detect {
  file_exists "CMakeLists.txt"
}

requires {
  any_of   [cmake]
  optional [ninja, make, ccache]
}

schema {
  generator:   enum { auto, ninja, make, unix_makefiles } = auto
  build_dir:   str  = "build"
  cmake_args:  list<str> = []
}

command build(project, config, extra) {
  let dir = config.build_dir

  step mkdir "-p" dir

  let cmake = ["cmake", "-S", ".", "-B", dir]

  let gen = match config.generator {
    auto            => project.tool("ninja") ? "Ninja" : none,
    ninja           => "Ninja",
    make            => "Unix Makefiles",
    unix_makefiles  => "Unix Makefiles",
  }

  if gen != none {
    cmake = cmake + ["-G", gen]
  }

  cmake = cmake + config.cmake_args
  step cmake

  let build = ["cmake", "--build", dir] + extra
  step build
}

command clean(project, config) {
  step rm "-rf" config.build_dir
  report_freed_space
}

command test(project, config, extra) {
  step ["ctest", "--test-dir", config.build_dir] + extra
}
```

### 5.4 The `CommandSpec` return contract

KPL `command` blocks do not return values explicitly — they **accumulate
steps** via `step` statements. The interpreter builds:

```json
{
  "steps": [
    { "cmd": ["cmake", "-S", ".", "-B", "build"], "cwd": null, "env": {}, "label": null },
    { "cmd": ["cmake", "--build", "build", "--release"], "cwd": null, "env": {}, "label": null }
  ],
  "concurrent": false,
  "report_freed_space": false
}
```

Step forms:

```kpl
step mkdir "-p" "build"                    // variadic argv: strings and expressions
step ["cargo", "build"] + extra            // list<string> expression
step { cmd: ["npm", "run", "dev"], cwd: "packages/app", label: "app" }  // record form
```

Modifiers (block-level):

```kpl
concurrent true          // executor runs all steps in parallel (for `dev`)
report_freed_space       // `kap clean` reports bytes freed
```

**Plugins never spawn processes.** A `step` appends to the spec; only the
C++ executor calls `posix_spawn`. This is what makes `--dry-run`, logging,
and sandboxing uniform.

### 5.5 KPL grammar (EBNF)

```ebnf
file        = { top_level_decl } ;

top_level_decl
            = manifest_block | detect_block | requires_block
            | schema_block | command_decl ;

manifest_block = "manifest" block ;
detect_block   = "detect" block ;
requires_block = "requires" block ;
schema_block   = "schema" block ;

command_decl   = "command" IDENT "(" param_list ")" block ;

block       = "{" { stmt } "}" ;

stmt        = let_stmt | step_stmt | if_stmt | for_stmt | match_stmt
            | concurrent_stmt | report_stmt ;

let_stmt    = "let" IDENT "=" expr ;
step_stmt   = "step" step_arg { step_arg } | "step" expr ;
if_stmt     = "if" expr block { "else" "if" expr block } [ "else" block ] ;
for_stmt    = "for" IDENT "in" expr block ;
match_stmt  = "match" expr "{" { match_arm } "}" ;
match_arm   = pattern "=>" expr "," ;

concurrent_stmt = "concurrent" BOOL ;
report_stmt     = "report_freed_space" ;

step_arg    = STRING | expr ;

param_list  = [ param { "," param } ] ;
param       = IDENT ;

expr        = assign_expr ;
assign_expr = or_expr ;
or_expr     = and_expr { "||" and_expr } ;
and_expr    = eq_expr { "&&" eq_expr } ;
eq_expr     = cmp_expr { ( "==" | "!=" ) cmp_expr } ;
cmp_expr    = add_expr { ( "<" | ">" | "<=" | ">=" ) add_expr } ;
add_expr    = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr    = unary_expr { ( "*" | "/" ) unary_expr } ;
unary_expr  = ( "!" | "-" ) unary_expr | postfix_expr ;
postfix_expr= primary_expr { "." IDENT | "(" arg_list ")" | "[" expr "]" } ;
primary_expr= literal | IDENT | "(" expr ")" | list_expr | record_expr
            | "if" expr expr "else" expr ;

pattern     = literal | IDENT | "none" ;

literal     = STRING | INT | BOOL | "none" ;
list_expr   = "[" [ expr { "," expr } ] "]" ;
record_expr = "{" field { "," field } "}" ;
field       = IDENT ":" expr ;
arg_list    = [ expr { "," expr } ] ;
```

Lexical rules:
- `IDENT`: `[A-Za-z_][A-Za-z0-9_]*`
- `STRING`: `"..."` with escapes `\"`, `\\`, `\n`, `\t`
- `INT`, `BOOL` (`true`/`false`)
- `//` to end-of-line comments
- Whitespace-insensitive

### 5.6 Type system

KPL is statically typed at plugin-load time (not at `kap` compile time).
The interpreter validates each `command` block before it can run.

| Type | Literals / forms | Notes |
|---|---|---|
| `str` | `"hello"` | UTF-8, immutable |
| `int` | `42` | 64-bit signed |
| `bool` | `true`, `false` | |
| `none` | `none` | Absence / optional unset |
| `list<T>` | `["a", "b"]`, concatenation `a + b` | Homogeneous |
| `enum` | declared in `schema` | Closed set, validated in config merge |
| `record` | `{ cmd: [...], cwd: "x" }` | Step spec or ad-hoc struct |
| `project` | `project` param only | Host object, read-only |
| `config` | `config` param only | Merged config record for this plugin |

Config values are injected as a `config` record whose fields match the
`schema` block. Unknown config keys are rejected at merge time.

### 5.7 Schema block

Schemas declare user-overridable keys with types and defaults:

```kpl
schema {
  generator:   enum { auto, ninja, make, unix_makefiles } = auto
  build_dir:   str  = "build"
  cmake_args:  list<str> = []
  release:     bool = false
}
```

Supported field types: `str`, `int`, `bool`, `list<str>`, `list<int>`,
`enum { ... }`. The core validates `~/.config/kap/config.toml` and
`./kap.toml` against this schema *before* invoking KPL, so typos fail
fast with a clear error.

### 5.8 Built-in operators and stdlib functions

**Operators:** `+` (list concat, string concat), `==`, `!=`, `<`, `>`,
`<=`, `>=`, `&&`, `||`, `!`, `-` (unary negation on int).

**Host builtins** (available in expressions):

| Function | Signature | Description |
|---|---|---|
| `project.exists` | `(path: str) -> bool` | File/dir exists under root |
| `project.read` | `(path: str) -> str` | Read file, max 1 MiB |
| `project.glob` | `(pattern: str) -> list<str>` | Glob relative to root |
| `project.tool` | `(name: str) -> bool` | Executable on PATH |
| `project.env` | `(name: str) -> str?` | Filtered env var |
| `len` | `(list) -> int` | List length |
| `contains` | `(haystack: str, needle: str) -> bool` | Substring test |
| `trim` | `(s: str) -> str` | Strip whitespace |
| `split` | `(s: str, sep: str) -> list<str>` | String split |

No user-defined functions in v1 — only `command` blocks. This keeps the
interpreter small and makes plugins easy to audit.

### 5.9 Control flow

```kpl
// if / else if / else
if project.tool("ninja") {
  step ninja
} else {
  step make
}

// for — iterate list only (no numeric ranges in v1)
for ws in project.glob("packages/*") {
  if project.exists(ws + "/package.json") {
    step { cmd: ["npm", "run", "dev"], cwd: ws, label: ws }
  }
}

// match — exhaustiveness checked when arms cover enum
let pm = match config.package_manager {
  npm  => "npm",
  pnpm => "pnpm",
  yarn => "yarn",
}

// inline conditional
let flag = if config.release then "--release" else none
```

### 5.10 Example — `cargo-rust`

```kpl
manifest {
  name        = "cargo-rust"
  version     = "0.9.1"
  api_version = 1
  priority    = 40
}

detect {
  file_exists "Cargo.toml"
}

requires {
  any_of   [cargo]
  optional [rustfmt, clippy]
}

schema {
  release: bool = false
}

command build(project, config, extra) {
  let cmd = ["cargo", "build"]
  if config.release { cmd = cmd + ["--release"] }
  cmd = cmd + extra
  step cmd
}

command test(project, config, extra) {
  step ["cargo", "test"] + extra
}

command lint(project, config) {
  step ["cargo", "clippy", "--", "-D", "warnings"]
}

command fmt(project, config) {
  let args = if config.check then ["--", "--check"] else []
  step ["cargo", "fmt"] + args
}

command clean(project, config) {
  step ["cargo", "clean"]
  report_freed_space
}
```

### 5.11 Example — `node` with workspace `dev`

```kpl
command dev(project, config) {
  let pkg = project.read("package.json")

  if contains(pkg, "\"workspaces\"") && config.dev_all_workspaces {
    concurrent true
    for ws in project.glob("packages/*") {
      if project.exists(ws + "/package.json") {
        let pm = config.package_manager
        step { cmd: [pm, "run", "dev"], cwd: ws, label: ws }
      }
    }
  } else {
    step [config.package_manager, "run", "dev"]
  }
}
```

### 5.12 Customization layers (ninja vs make)

Three layers, none requiring a plugin fork:

1. **Plugin default** (`auto` generator logic in `plugin.kpl`).
2. **User global override** (`~/.config/kap/config.toml`):
   ```toml
   [plugins.cmake-cpp]
   generator = "make"
   ```
3. **Per-project override** (`./kap.toml`, committed):
   ```toml
   [plugins.cmake-cpp]
   generator = "ninja"
   build_dir = "out"
   cmake_args = ["-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
   ```

Resolution order (later wins): **schema defaults → global config →
project config → CLI flag** (`kap build --set generator=ninja`).

### 5.13 Hooks (pre/post)

```toml
# kap.toml
[hooks]
pre_build = "echo starting build..."
post_test = "notify-send 'tests finished'"
```

Hooks are plain shell strings run by the executor, not KPL — they cover
"run one extra command" without learning the DSL.

### 5.14 KPL compilation cache

On plugin install or first load, the core parses `plugin.kpl` to an AST and
writes a binary cache:

```
~/.cache/kap/ast/<name>@<version>.kapc
```

The cache is invalidated when `plugin.kpl` mtime or `api_version` changes.
Detection rules are extracted from the AST at load time into a C++
`DetectRuleTable` so the hot path never re-parses text.

---

## 6. The Plugin Manager

### 6.1 Commands

| Command | Behavior |
|---|---|
| `kap plugin list` | Show installed plugins, source, version, enabled/disabled |
| `kap plugin search <query>` | Query the registry index |
| `kap plugin install <name\|git-url\|path>` | Install from registry, git, or local path |
| `kap plugin install --bundle <name>` | Install a curated set of build-system plugins |
| `kap plugin new <name> [--template build-system]` | Scaffold a new KPL plugin |
| `kap plugin update [name]` | Update one or all plugins |
| `kap plugin remove <name>` | Uninstall |
| `kap plugin enable/disable <name>` | Toggle without uninstalling |
| `kap plugin pin <name> <version>` | Lock a version |
| `kap plugin test <name>` | Run fixture tests (assert on returned steps) |
| `kap plugin doctor` | Validate every installed plugin against `api_version` |

### 6.2 Install sources

- **Registry**: static git-hosted `index.toml` (no custom server).
- **Direct git URL**: `kap plugin install https://github.com/user/kap-cmake-cpp`
- **Local path**: `kap plugin install --link ./my-plugin` (symlink for dev).
- **Bundles**: `kap plugin install --bundle cpp`

### 6.3 Install pipeline

```
kap plugin install cmake-cpp
   │
   ├─ 1. Resolve source (registry / git URL / local path)
   ├─ 2. Fetch payload (shallow git clone or symlink)
   ├─ 3. Validate:
   │      - plugin.kpl parses without error
   │      - manifest fields present (name, version, api_version)
   │      - api_version <= kap's supported max
   │      - smoke-eval: every declared command type-checks
   │      - checksum matches (registry installs)
   ├─ 4. Atomic install to ~/.local/share/kap/plugins/cmake-cpp/
   ├─ 5. Update lockfile installed-plugins.toml
   ├─ 6. Pre-compile AST cache
   └─ 7. Invalidate detection cache
```

### 6.4 On-disk layout

```
~/.local/share/kap/
├── installed-plugins.toml
├── plugins/
│   ├── cmake-cpp/
│   │   ├── plugin.kpl
│   │   └── README.md
│   └── cargo-rust/
└── registry/
    └── index.toml

~/.cache/kap/
├── ast/                    # compiled KPL AST blobs
└── plugins-src/            # ephemeral git clones
```

### 6.5 Override precedence

```
./.kap/plugins/<name>/              (project-local)
~/.local/share/kap/plugins/<name>/  (user-installed)
<prefix>/share/kap/plugins/<name>/  (bundled defaults)
```

### 6.6 Bundled plugins

`kap` ships a minimal set of plugins in its data directory — `doctor`,
`ports`, `generic-makefile` — written in KPL, not C++. This dogfoods the
same DSL external authors use.

---

## 7. Security & Sandboxing

KPL plugins are third-party code fetched from git repos — treat them like
untrusted packages.

- **No process execution in KPL.** Only `step` declarations; the C++
  executor is the sole spawner.
- **No raw filesystem access.** Only `project.exists`, `project.read`,
  `project.glob` — path-canonicalized, refuse to escape `project.root`.
- **No imports or includes** in v1. One `plugin.kpl` per plugin.
- **Filtered environment.** `project.env` uses a deny-list (`*_TOKEN`,
  `*_KEY`, `AWS_*`, ...).
- **Bounded reads.** `project.read` capped at 1 MiB; `project.glob`
  capped at 10 000 results.
- **No loops over unbounded user input** without caps — `for` iterates
  only lists the host returns (already capped).
- **`kap plugin install`** prints a summary and requires confirmation
  unless `--yes`.
- **`--dry-run`** always shows the exact argv arrays before execution.

---

## 8. CLI Surface (v1 parity with `uu`)

| Command | Notes |
|---|---|
| `kap build` | |
| `kap check` | typecheck-only |
| `kap ci` | fmt-check + lint + test, or plugin-defined |
| `kap clean` | |
| `kap dev` | concurrent multi-step specs |
| `kap doctor` | bundled KPL plugin |
| `kap fmt` | |
| `kap install` | |
| `kap lint` | |
| `kap ports` | bundled KPL plugin |
| `kap run` | |
| `kap test` | |
| `kap plugin ...` | plugin manager, §6 |
| `kap config ...` | `get`/`set`/`edit` |
| global flags | `--dry-run`/`-n`, `--root`, `--set key=value`, `--verbose` |

---

## 9. Tech Stack — Zero-Dependency Rule

**Rule:** The `kap` binary links only against the C++ standard library and
POSIX (`-pthread` where needed). No vendored third-party C/C++ libraries.
No package managers (vcpkg, Conan) in the build graph.

| Concern | Choice | Notes |
|---|---|---|
| Language | C++20 | `std::filesystem`, `std::span`, `std::optional`, concepts |
| CLI parsing | In-tree minimal parser | Single header + `.cpp`, no CLI11 |
| Config format | Minimal TOML subset parser (in-tree) | Enough for `kap.toml`, lockfiles, registry index |
| Plugin language | **KPL** (in-tree lexer/parser/interpreter) | Replaces Lua/sol2 entirely |
| Process execution | POSIX `posix_spawn` / `fork`+`execve` | argv-array based, no `system()` |
| Plugin fetch | `posix_spawn` of system `git` | Git is a *runtime* tool, not a link dependency |
| Filesystem | `std::filesystem` + POSIX `stat` | |
| String/format | `std::string`, `std::format` (or small polyfill) | |
| Hashing (cache keys) | In-tree FNV-1a or similar | No OpenSSL requirement |
| Build system for kap itself | CMake + Ninja | CMake is a *dev* tool, not linked |
| Testing | In-tree micro test harness | `kap_test` macro, no Catch2/GTest |
| Containers | Docker (dev only) | Not linked; synchronizes dev environment |

**What is allowed at runtime (not linked):**
- `git` — plugin install
- Ecosystem tools (`cmake`, `cargo`, `npm`, ...) — what plugins invoke

**What is NOT allowed:**
- Linking Boost, fmt, spdlog, sol2, Lua, toml++, nlohmann/json, etc.
- Bundling a JS/WASM runtime

---

## 10. Developer Environment (Docker)

Every contributor must be able to reproduce the exact same toolchain with
one command. Local bare-metal builds are supported but CI and onboarding
use Docker as the source of truth.
Give a detailed guide on how to use docker to make a usable dev environment in a file called "dockerusage.md"

### 10.1 Repository layout

```
kap/
├── Dockerfile              # release-ish image (multi-stage)
├── docker/
│   ├── dev.Dockerfile      # fat dev image with all ecosystem tools
│   └── entrypoint.sh       # drops into shell with build env set
├── docker-compose.yml      # `docker compose run dev` is the default onboarding path
├── .dockerignore
├── CMakeLists.txt
├── core/                   # C++ source
├── plugins/                # first-party KPL plugins
├── registry/               # plugin index
└── scripts/
    ├── in-docker.sh        # wrapper: `./scripts/in-docker.sh cmake --build build`
    └── bootstrap.sh        # first-time setup inside container
```

### 10.2 `docker-compose.yml` (dev)

Improve it as per need

```yaml
services:
  dev:
    build:
      context: .
      dockerfile: docker/dev.Dockerfile
    volumes:
      - .:/kap
      - kap-build:/kap/build
      - kap-cache:/root/.cache/kap
    working_dir: /kap
    environment:
      - KAP_DEV=1
    stdin_open: true
    tty: true

volumes:
  kap-build:
  kap-cache:
```

### 10.3 `docker/dev.Dockerfile`

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates \
    # ecosystem tools for dogfooding first-party plugins:
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /kap
COPY scripts/bootstrap.sh /usr/local/bin/kap-bootstrap
RUN kap-bootstrap --check-deps

ENTRYPOINT ["/kap/docker/entrypoint.sh"]
CMD ["bash"]
```

### 10.4 Daily workflow

```bash
# First time
git clone https://github.com/kap-project/kap
cd kap
docker compose run --rm dev

# Inside container (or via wrapper)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/kap --help

# Run tests — same inside or outside container via wrapper
./scripts/in-docker.sh ctest --test-dir build

# Iterate on a plugin
kap plugin install --link ./plugins/cmake-cpp
kap plugin test cmake-cpp
```

### 10.5 Synchronization guarantees

| Concern | Mechanism |
|---|---|
| Compiler version | Pinned base image digest in `docker/dev.Dockerfile` |
| CMake/Ninja version | Installed in image, not host |
| Test toolchains | Optional image tags: `dev`, `dev-full` (adds Rust, Node, ...) |
| CI | GitHub Actions job uses the same `docker/dev.Dockerfile` |
| Host independence | `./scripts/in-docker.sh <cmd>` bind-mounts repo, runs in container |

### 10.6 CI pipeline

```yaml
# .github/workflows/ci.yml (conceptual)
jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build and test in dev container
        run: |
          docker compose build dev
          docker compose run --rm dev ./scripts/ci.sh
```

`scripts/ci.sh` configures, builds, runs unit tests, runs `kap plugin test`
for all first-party plugins, and smoke-tests the binary.

---

## 11. Incremental Roadmap

Each milestone produces a **working, testable artifact**. No phase begins
until the previous phase's exit criteria pass in Docker CI.

### Milestone 0 — Repo + Docker dev shell
**Goal:** Anyone can clone and get a shell with pinned toolchain.

- [x] Repository skeleton (`core/`, `plugins/`, `registry/`, `docker/`)
- [x] `docker/dev.Dockerfile`, `docker-compose.yml`, `scripts/in-docker.sh`
- [x] Empty `kap` binary that prints version and exits 0
- [x] In-tree test harness compiles and runs one smoke test
- [x] CI: build container, compile, run smoke test

**Exit criteria:** `docker compose run --rm dev ./scripts/ci.sh` green on
a fresh clone.

---

### Milestone 1 — Minimal infrastructure libraries
**Goal:** Shared zero-dep utilities used by everything else.

- [x] `core/diag.hpp` — error types, source locations, rich messages
- [x] `core/argv.hpp` — argv vector, escaping for display
- [x] `core/fs.hpp` — thin `std::filesystem` wrappers (exists, read capped, glob)
- [x] `core/toml.hpp` — minimal TOML subset parser (tables, strings, ints,
      bools, arrays; enough for config + lockfile + registry index)
- [x] `core/cli.hpp` — subcommand parser with `--` passthrough
- [x] Unit tests for TOML parser and CLI parser

**Exit criteria:** `kap config get` reads a fixture TOML file; 100% of
parser tests pass. ✅ — 131 unit tests + 41 end-to-end assertions, all green
under `./scripts/ci.sh`.

**Notes for later milestones.** The TOML subset deliberately stops short of
full TOML 1.0. Not implemented, because nothing in the design needs it yet:
floats, datetimes, quoted/dotted *keys* (`"a.b" = 1`), inline tables
(`{ a = 1 }`), arrays of tables (`[[x]]`), literal `'single-quoted'` strings,
multi-line `"""` strings, `\uXXXX` escapes, and hex/octal/binary integers. Each
is rejected with a located diagnostic rather than misparsed, so adding one
later is additive and cannot silently change the meaning of an existing file.

`core/argv.hpp`'s `escape_word` renders for *display only* (`--dry-run`, error
messages). It is never used to build a command line for execution — the
executor in Milestone 5 passes argv arrays to `posix_spawn` directly, so no
quoting round-trip is involved and no shell is ever invoked.

---

### Milestone 2 — KPL lexer + parser
**Goal:** Parse `plugin.kpl` into an AST; no execution yet.

- [x] Tokenizer with comment support
- [x] Recursive-descent parser per §5.5 grammar
- [x] AST nodes for all top-level blocks and statements
- [x] `kap plugin doctor` reports parse errors with line/column
- [x] Golden tests: parse all example plugins in `plugins/`

**Exit criteria:** Every `.kpl` fixture parses; malformed inputs produce
diagnostics with line numbers. ✅

---

### Milestone 3 — KPL type-checker + interpreter
**Goal:** Evaluate `command` blocks and produce `CommandSpec` JSON.

- [ ] Schema validation (config record construction)
- [ ] Type checker for expressions and `step` statements
- [ ] Tree-walk interpreter with `project` host object (mocked in tests)
- [ ] `report_freed_space`, `concurrent` modifiers
- [ ] AST binary cache (`.kapc`)
- [ ] `kap plugin test <name>` — compare steps against `expected/*.json`

**Exit criteria:** `cmake-cpp` and `cargo-rust` fixture tests pass without
executing real build tools.

---

### Milestone 4 — Detection engine
**Goal:** Given installed plugins, pick the right one for a directory.

- [ ] Extract `detect` rules from AST at plugin load
- [ ] Rule evaluators: `file_exists`, `file_exists_any`, `file_contains`,
      `dir_exists`
- [ ] Priority + `supersedes` resolution, tie error path
- [ ] `.kap/cache.json` mtime-based cache
- [ ] `kap detect` debug subcommand (prints matched plugin + score)

**Exit criteria:** Synthetic fixture trees resolve to the expected plugin;
cache hit skips re-scan.

---

### Milestone 5 — Executor
**Goal:** Actually run commands.

- [ ] `posix_spawn` wrapper with stdout/stderr streaming
- [ ] `--dry-run` rendering (print argv arrays)
- [ ] Per-step `cwd`, `env`, `label`
- [ ] Concurrent mode with prefixed output (`kap dev`)
- [ ] Hook execution (`pre_*`/`post_*` from `kap.toml`)
- [ ] Exit-code propagation; `SIGINT` forwards to all children

**Exit criteria:** `kap build` in a real CMake project builds it;
`kap build -n` prints commands without running.

---

### Milestone 6 — Config merge + CLI wiring
**Goal:** End-to-end `kap <command>` for one plugin.

- [ ] Config merge: schema defaults → global → project → `--set`
- [ ] Wire CLI → detect → load plugin → eval KPL → execute
- [ ] `kap config get/set/edit`
- [ ] All v1 commands routed (missing command → clear error)

**Exit criteria:** `kap build/test/clean` work in a dogfood repo using
the `cmake-cpp` plugin.

---

### Milestone 7 — Plugin manager
**Goal:** Install plugins from registry/git/local.

- [ ] Registry `index.toml` format + fetch/cache
- [ ] `posix_spawn` git clone (shallow, pinned ref)
- [ ] Install pipeline: fetch → validate (parse + smoke typecheck) → atomic install → lockfile
- [ ] `install`, `remove`, `update`, `enable`/`disable`, `pin`, `list`, `search`
- [ ] `kap plugin new` scaffolds from template
- [ ] `--link` for local development
- [ ] Override precedence (project-local > user > bundled)

**Exit criteria:** Fresh `kap` install + `kap plugin install cmake-cpp` +
`kap build` works in a clean container.

---

### Milestone 8 — First-party plugins (core ecosystems)
**Goal:** Parity with the most common `uu` ecosystems.

Priority order:
1. `cmake-cpp`
2. `make-generic`
3. `cargo-rust`
4. `node` (workspace-aware `dev`)
5. `go`
6. `python-uv`

Each plugin ships `plugin.kpl` + fixture tests. No plugin merges without
`kap plugin test` passing.

**Exit criteria:** `kap plugin install --bundle core` enables build/test in
six fixture project types.

---

### Milestone 9 — Bundled system plugins
**Goal:** `doctor` and `ports` in KPL.

- [ ] `doctor` — checks `[requires]` from all matched plugins
- [ ] `ports` — reads `/proc/net/tcp` (Linux) or `lsof` fallback via step
- [ ] `generic-makefile` fallback plugin

**Exit criteria:** `kap doctor` and `kap ports` work; implemented entirely
in KPL.

---

### Milestone 10 — Polish + v1.0
**Goal:** Usable by early adopters.

- [ ] `dev` concurrency polish (colored prefixes, `-o` open-first-URL)
- [ ] Shell completions (bash/zsh/fish)
- [ ] `PLUGIN_API.md` generated from this doc's KPL section
- [ ] `docker/dev-full.Dockerfile` with Rust, Node, Go for full bundle tests
- [ ] Install script (`curl | sh` clones registry + installs binary)
- [ ] v1.0 tag, registry published

**Exit criteria:** Install script works on Linux; docs cover plugin
authoring, config reference, and Docker workflow.

---

### Post-v1 backlog (not blocking release)

- Windows port (executor + path semantics; KPL unchanged)
- `dev-full` image with all ecosystem toolchains
- Plugin signing / checksum policy hardening
- Numeric `for i in 0..N` in KPL if a real plugin needs it
- Optional `import` of shared KPL snippets (`.kpli`) with allow-list

---

## 12. Open Questions

1. **Windows executor** — `posix_spawn` is POSIX-only. v1 targets Linux;
   Windows needs a `CreateProcess` backend behind the same `Process` API.
2. **Plugin API versioning** — semver on `api_version`; hard error vs
   warn-and-skip when a plugin is too new.
3. **Registry trust** — checksum per version in `index.toml` (current
   plan) vs. signed index. Decide before Milestone 7.
4. **`glob` implementation** — `std::filesystem` has no glob; roll a
   minimal `fnmatch` wrapper via POSIX `fnmatch(3)` (allowed) vs. pure
   C++ recursive directory walk.
5. **TOML vs KPL for config** — user config stays TOML (familiar,
   editable); only plugins use KPL. Alternative: a `config.kpl` — rejected
   for v1 because TOML is better for non-programmers.

---

## 13. Glossary

| Term | Meaning |
|---|---|
| **kap** | The CLI binary and project name |
| **KPL** | Kap Plugin Language — `.kpl` files |
| **Plugin** | A directory with `plugin.kpl` + README |
| **CommandSpec** | JSON-like step list the executor runs |
| **Zero-dependency** | No third-party libs linked into `kap`; stdlib + POSIX only |
| **Registry** | Git-hosted `index.toml` of available plugins |
