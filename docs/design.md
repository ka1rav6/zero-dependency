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

**Case format.** One file under `expected/` is one test case. `kap plugin test`
evaluates a command block against a fixture directory and compares the
resulting `CommandSpec` (§5.4) with the file. Nothing is executed.

The file name binds the two halves by convention —
`<fixture>.<command>.steps.json` — and the JSON may override either half
explicitly, which is what lets one command have several cases:

```json
{
  "fixture": "simple-project",     // optional; else from the file name, else
                                   // the plugin's single fixture directory
  "command": "build",              // optional; else the last dotted component
  "config":  { "build_dir": "out" },  // optional overrides on schema defaults
  "extra":   ["--release"],           // optional passthrough arguments
  "tools":   ["cmake", "ninja"],      // what project.tool() reports present
  "env":     { "CI": "true" },        // what project.env() returns

  "steps": [ { "cmd": ["cmake", "--build", "out"] } ],
  "concurrent": false,
  "report_freed_space": false
}
```

`tools` and `env` are **declared, not sampled from the host**. §5.1 promises
the same inputs always yield the same `CommandSpec`; a case that consulted the
real `PATH` would pass or fail depending on the machine. Everything else —
`exists`, `read`, `glob` — comes from the fixture directory through the normal
§7 sandbox, which is what a fixture is for.

Fields omitted from a step (`cwd`, `env`, `label`) and from the spec
(`concurrent`, `report_freed_space`) take their defaults, so a golden file only
spells out what it cares about.

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
    auto            => if project.tool("ninja") then "Ninja" else none,
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

**Bare words.** In the first form, `mkdir` is a program name and `dir` in
`step mkdir "-p" dir` is a variable — both are bare identifiers. The rule that
makes both work: an identifier standing alone as a `step` argument resolves to
a variable if one is bound, and is otherwise the literal word. This applies
*only* in that position; `step [name]` and `step name + ".txt"` still require a
real binding, so the cost (a mistyped variable silently becoming a word) is
confined to the one place the syntax needs it.

In the variadic form a string contributes one word and a list splices in all of
its words, so `step "ninja" dir` and `step ["ninja", dir]` are the same argv.
The record form is the only way to set a step's `cwd`, `env`, or `label`;
mixing it with other arguments is an error, and an unrecognised field is
rejected rather than ignored.

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
detect_block   = "detect" directive_block ;
requires_block = "requires" directive_block ;
schema_block   = "schema" block ;

(* `detect` and `requires` hold declarations, not statements. A directive
   argument is deliberately never a bare identifier, so an identifier at
   argument position unambiguously starts the next directive — no lookahead,
   no newline sensitivity. Bare identifiers *inside* a list are read as
   strings, which is why `any_of [cmake]` needs no quotes. *)
directive_block = "{" { directive } "}" ;
directive       = IDENT { directive_arg } ;
directive_arg   = STRING | INT | directive_list | directive_record ;
directive_list  = "[" [ directive_elem { "," directive_elem } ] "]" ;
directive_record= "{" field_lit { "," field_lit } "}" ;
field_lit       = IDENT ":" directive_elem ;
directive_elem  = STRING | INT | BOOL | IDENT ;

command_decl   = "command" IDENT "(" param_list ")" block ;

block       = "{" { stmt } "}" ;

stmt        = let_stmt | assign_stmt | step_stmt | if_stmt | for_stmt
            | match_stmt | concurrent_stmt | report_stmt ;

let_stmt    = "let" IDENT "=" expr ;
step_stmt   = "step" step_arg { step_arg } | "step" expr ;
if_stmt     = "if" expr block { "else" "if" expr block } [ "else" block ] ;
for_stmt    = "for" IDENT "in" expr block ;
(* `match` is an EXPRESSION (§5.9 assigns one to a `let`); match_stmt is just
   one in statement position, whose value is discarded. There is no catch-all
   pattern in v1 — an uncovered value is a located run-time error, which is the
   honest outcome when an enum member is added and an arm is forgotten. *)
match_expr  = "match" expr "{" match_arm { match_arm } "}" ;
match_stmt  = match_expr ;
match_arm   = pattern "=>" expr [ "," ] ;
assign_stmt = IDENT "=" expr ;

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
            | match_expr
            | "if" expr "then" expr "else" expr ;

(* A bare IDENT pattern is an enum MEMBER compared as its own text, not a
   variable read: `ninja => "Ninja"` never binds anything. *)
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

The type checker reads the same block, so inside a command block
`config.<field>` has the field's declared type rather than being opaque. Three
consequences: a `config` key that is not in the schema is a load-time error
naming the key; element types survive concatenation, so
`["cmake"] + config.cmake_args` type-checks as `list<str>`; and a `match` over
an `enum`-typed field is checked for exhaustiveness (§5.9). A plugin with no
`schema` block declares no config surface, so its `config` reads are unchecked
rather than reported as unknown keys.

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
| `kap plugin test <name>` | Run fixture tests (assert on returned steps) — implemented, §5.2 |
| `kap plugin doctor` | Parse, manifest-validate, and type-check every installed plugin — implemented |

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
├── kap-plugins/
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
`ports`, `make-generic` — written in KPL, not C++. This dogfoods the
same DSL external authors use.

(Milestone 8's list spells the Makefile plugin `make-generic` and this section
originally spelled it `generic-makefile`. `make-generic` is the name that
ships.)

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
| JSON | Minimal in-tree subset (`core/json.hpp`) | CommandSpec golden files (§5.2), `.kap/cache.json` (§3.2). No floats, no `\uXXXX` — both rejected, never misparsed |
| Plugin language | **KPL** (in-tree lexer/parser/interpreter) | Replaces Lua/sol2 entirely |
| Process execution | POSIX `posix_spawn` / `fork`+`execve` | argv-array based, no `system()` |
| Plugin fetch | `posix_spawn` of system `git` | Git is a *runtime* tool, not a link dependency |
| Filesystem | `std::filesystem` + POSIX `stat` | |
| String/format | `std::string`, `std::format` (or small polyfill) | |
| Hashing (cache keys) | In-tree FNV-1a (`core/hash.hpp`) | No OpenSSL requirement. Cache keys only, never a trust decision |
| Hashing (registry checksums) | In-tree SHA-256 (`core/sha256.hpp`) | §6.3's integrity gate *is* a trust decision, so it needs a hash that is not forgeable. Still no OpenSSL: §9 bans linking a library, not computing a digest |
| KPL AST cache | In-tree little-endian encoding (`core/kapc.hpp`) | §5.14. Magic + format version + bounds-checked decode; any problem falls back to parsing |
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
├── kap-plugins/            # first-party KPL plugins
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
git clone https://github.com/ka1rav6/zero-dependency
cd kap
docker compose run --rm dev

# Inside container (or via wrapper)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/kap --help

# Run tests — same inside or outside container via wrapper
./scripts/in-docker.sh ctest --test-dir build

# Iterate on a plugin
kap plugin install --link ./kap-plugins/cmake-cpp
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

- [x] Repository skeleton (`core/`, `kap-plugins/`, `registry/`, `docker/`)
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
- [x] Golden tests: parse all example plugins in `kap-plugins/`

**Exit criteria:** Every `.kpl` fixture parses; malformed inputs produce
diagnostics with line numbers. ✅

---

### Milestone 3 — KPL type-checker + interpreter
**Goal:** Evaluate `command` blocks and produce `CommandSpec` JSON.

- [x] Schema validation (config record construction)
- [x] Type checker for expressions and `step` statements
- [x] Tree-walk interpreter with `project` host object (mocked in tests)
- [x] `report_freed_space`, `concurrent` modifiers
- [x] AST binary cache (`.kapc`)
- [x] `kap plugin test <name>` — compare steps against `expected/*.json`

**Exit criteria:** `cmake-cpp` and `cargo-rust` fixture tests pass without
executing real build tools. ✅ — 7 fixture cases across the two bundled
plugins, green under `./scripts/ci.sh` on a machine with no cmake, cargo, or
ninja installed. 222 unit tests + 59 end-to-end assertions, clean under
`-DKAP_SANITIZE=ON`.

**Notes for later milestones.**

The interpreter implements the whole of §5.8 (`project.exists/read/glob/tool/
env`, `len`, `contains`, `trim`, `split`) and all of §5.9's control flow.
`host_project()` in `core/kpl.cpp` is where §7's sandbox lives: one choke point
canonicalises every plugin-supplied path and refuses anything outside the
project root, reads inherit `fs::read_text`'s 1 MiB cap, globs inherit
`fs::glob`'s 10 000-result cap and only wildcard the final path component, and
`project.env` applies a deny-list (`*_TOKEN`, `*_KEY`, `*_SECRET`,
`*_PASSWORD`, `AWS_*`, ...). `project.tool` scans `PATH` with `access(X_OK)`
and never spawns.

Two v1 limits worth knowing before writing a plugin. There is no catch-all
`match` pattern, so a `match` over a `str` is only usable when the arms cover
every value the plugin can produce; over a schema `enum` the checker proves
coverage. And `for` iterates lists only — the numeric-range form is in the
post-v1 backlog and no bundled plugin needs it.

Milestone 4 inherits two things ready to use: `detect` blocks now parse into
directive statements (name + arguments) that compile straight to a
`DetectRuleTable`, and `core/json.hpp` is the parser `.kap/cache.json` needs.

---

### Milestone 4 — Detection engine
**Goal:** Given installed plugins, pick the right one for a directory.

- [x] Extract `detect` rules from AST at plugin load
- [x] Rule evaluators: `file_exists`, `file_exists_any`, `file_contains`,
      `dir_exists`
- [x] Priority + `supersedes` resolution, tie error path
- [x] `.kap/cache.json` mtime-based cache
- [x] `kap detect` debug subcommand (prints matched plugin + score)

**Exit criteria:** Synthetic fixture trees resolve to the expected plugin;
cache hit skips re-scan. ✅ — 33 detection unit tests + 17 end-to-end
assertions, green under `./scripts/ci.sh`.

**Notes for later milestones.**

`core/detect.hpp` is the engine; `core/plugin.hpp`'s `discover()` now
implements §6.5's override precedence in full (project-local > $KAP_PLUGIN_PATH
> user-installed > bundled > the repository's own `kap-plugins/`), so Milestone 7
only has to *write* into those directories, not teach anything how to find
them. `core/paths.hpp` resolves every XDG location in one place.

Three points where the implementation is more specific than §3 could be:

*Score vs. priority.* §3.2 uses "score" for what a plugin's satisfied rules
contribute and "priority" for the manifest number. They are separate fields:
ranking is by `priority`, and `score` (how many rules fired) is reported by
`kap detect` as diagnostic detail. A plugin matching more rules does not
out-rank a higher-priority one.

*Cache invalidation.* §3.2 says "keyed by a hash of the matched marker files'
mtimes". That alone cannot see a *newly added* marker — before the change there
were no markers to hash — so the cache has two guards: a precheck key over the
root's directory listing plus every candidate plugin's manifest mtime, and the
recorded per-marker mtimes re-verified on read. The listing is used rather than
the root's own mtime because writing the cache creates `.kap/`, which bumps
that mtime and would make the cache invalidate itself on every single write.
`kap detect --refresh` bypasses the entry for the case neither guard sees: a
marker created deep inside a subdirectory that no existing rule watches.

*Detect rules are sandboxed.* A `detect` rule runs before the interpreter's §7
sandbox exists, so `core/detect.cpp` applies the same containment itself: an
absolute or `../`-escaping marker path simply never matches. Otherwise a plugin
could map the filesystem above a project just by watching which plugin wins.

*§12 Q2 is settled* (see §11 Milestone 7 notes): a plugin declaring an
`api_version` newer than `detect::kSupportedApiVersion` is a hard error where
the user named it explicitly and a warn-and-skip during detection, so one
too-new plugin cannot break `kap build` in an unrelated project.

---

### Milestone 5 — Executor
**Goal:** Actually run commands.

- [x] Process wrapper with stdout/stderr streaming
- [x] `--dry-run` rendering (print argv arrays)
- [x] Per-step `cwd`, `env`, `label`
- [x] Concurrent mode with prefixed output (`kap dev`)
- [x] Hook execution (`pre_*`/`post_*` from `kap.toml`)
- [x] Exit-code propagation; `SIGINT` forwards to all children

**Exit criteria:** `kap build` in a real CMake project builds it;
`kap build -n` prints commands without running. ✅ — reached jointly with
Milestone 6, which is what wires a command line to `exec::run`; see that
milestone's notes.

**Notes for later milestones.**

`core/exec.hpp` is the only place in kap that creates a process, which is what
makes §5.4's promise real: a plugin cannot run anything kap has not first
rendered as an argv array it was willing to show you.

*`fork` + `execvp`, not `posix_spawn`.* §9 permits either. A step may carry its
own `cwd` (§5.4), and giving a *spawned* process a different working directory
needs `posix_spawn_file_actions_addchdir_np` — a glibc extension musl and older
glibc lack. Between `fork` and `exec`, `chdir` is one portable line.

*No shell for steps; a shell for hooks.* Steps are argv arrays, so a directory
named `my project` reaches the tool intact and a plugin cannot smuggle
`; rm -rf ~` through a filename. Hooks (§5.13) are explicitly "plain shell
strings", come from the user's own kap.toml rather than from a plugin, and run
through `/bin/sh -c`. `run_steps` and `run_hook` are separate functions so the
distinction cannot erode.

*Sequential runs inherit the terminal; concurrent runs use pipes.* An inherited
terminal keeps `cargo build`'s colours and `ninja`'s progress line working
exactly as if you had typed the command. Concurrent steps cannot have that —
four interleaved dev servers are unreadable — so they get pipes and each line
is emitted with its step's label as a coloured prefix. Multiplexing uses
`poll(2)` in the main thread rather than a thread per child, so the process
never forks while threads exist.

*A missing tool is not exit 127.* The child reports exec failure through a
close-on-exec pipe, so kap can say "cannot run 'cmake': No such file or
directory" instead of a bare status. A child killed by signal N is reported as
128 + N, the convention every POSIX shell uses.

*`report_freed_space` measures what it can name.* Any argv word that names an
existing path under the root is measured before and after; a command with no
path-shaped argument (`cargo clean`) falls back to measuring the whole root.
Symlinks are counted as links and never followed, so a link into `/usr` can
never be reported as freed space.

---

### Milestone 6 — Config merge + CLI wiring
**Goal:** End-to-end `kap <command>` for one plugin.

- [x] Config merge: schema defaults → global → project → `--set`
- [x] Wire CLI → detect → load plugin → eval KPL → execute
- [x] `kap config get/set/edit`
- [x] All v1 commands routed (missing command → clear error)

**Exit criteria:** `kap build/test/clean` work in a dogfood repo using
the `cmake-cpp` plugin. ✅ — end-to-end assertions build a real CMake project,
run the binary it produced, clean it, and check that a failing build propagates
its exit code. 325 unit tests + 112 end-to-end assertions.

**Notes for later milestones.**

`core/config.hpp` implements §5.12's four layers. Three decisions worth knowing:

*A nearer layer replaces, it never element-merges.* If `cmake_args` merged
element-wise, a project could never *remove* an argument its user's global
config had added — only add more. "The nearer layer wins outright" is the rule
a user can predict.

*`--set` is coerced through the schema.* A command line has no types, so the
schema is the only thing that can say whether `release=true` means the string
or the boolean. `--set` on a `list<str>` splits on commas, because §5.12's
syntax is one `key=value` pair and a list-valued key still has to fit in one.

*`kap config get` reads the merged view by default*, because "what will kap
actually do" is the question a user has; `--global` / `--project` narrow it to
one file. `kap config set` writes one file (project by default) through
`toml::write`, which round-trips *values* but not comments or layout — which is
why `kap config edit` exists.

`kap ci` implements §8's "fmt-check + lint + test, or plugin-defined"
literally: a plugin's own `ci` command wins outright, and otherwise kap runs
whichever of `fmt`, `lint`, and `test` the plugin defines, stopping at the
first failure. For the `fmt` phase only, if the plugin's schema declares a bool
field named `check`, kap sets it — that is the difference between a CI job
*verifying* formatting and one silently rewriting the checkout.

Post hooks run only on success. A `post_test = "notify-send 'tests finished'"`
firing after a *failed* test run would be actively misleading.

---

### Milestone 7 — Plugin manager
**Goal:** Install plugins from registry/git/local.

- [x] Registry `index.toml` format + fetch/cache
- [x] git clone (shallow, pinned ref)
- [x] Install pipeline: fetch → validate (parse + smoke typecheck) → atomic install → lockfile
- [x] `install`, `remove`, `update`, `enable`/`disable`, `pin`, `list`, `search`
- [x] `kap plugin new` scaffolds from template
- [x] `--link` for local development
- [x] Override precedence (project-local > user > bundled) — landed in Milestone 4

**Exit criteria:** Fresh `kap` install + `kap plugin install cmake-cpp` +
`kap build` works in a clean container. ✅ — `./scripts/ci.sh` exercises the
whole author loop (`new` → `doctor` → `test` → `install --link` → `list` →
`disable` → `remove`) through the real binary.

**Notes for later milestones.**

**§12 Q3 is answered: checksum, over SHA-256, enforced.** `core/sha256.hpp` is
a from-scratch FIPS 180-4 implementation — §9 bans linking OpenSSL, not
hashing, and a hundred lines of arithmetic with published test vectors is
exactly what the zero-dependency rule expects to be written in-tree.

It is deliberately a *different* function from `core/hash.hpp`'s FNV-1a, whose
own comment says it must never become a trust decision. A registry checksum is
precisely that: the only thing between a user and a tampered payload. FNV-1a is
trivially forgeable; SHA-256 is not.

The digest covers every regular file under the plugin directory, in sorted path
order, with the *paths* folded in and `.git` excluded. Paths matter because
hashing contents alone would give the same digest to a plugin whose
`plugin.kpl` and `README.md` had been swapped, and kap picks the file to
execute by name. `.git` is excluded because two shallow clones of the same
commit do not have identical object stores.

An index entry without a checksum installs, and kap says plainly that it could
not verify the payload rather than implying it did. A signed index stays
post-v1: it needs key distribution, which a static git repository does not
provide.

*What a checksum does not buy* is worth stating: it proves the payload matches
what the index author recorded, not that the plugin is safe to run. That is why
§7's confirmation prompt exists, and why a non-interactive stdin is treated as
"no" — installing third-party code because nobody was there to object is the
exact failure mode the prompt prevents.

Two other decisions:

*Enable/disable is recorded in the lockfile, not by moving files*, so it is
reversible and a switched-off plugin stays inspectable by `kap plugin doctor`.

*`kap plugin doctor` and `kap plugin test` accept a path, not just a name.*
`kap plugin new my-thing` followed by `kap plugin doctor my-thing` is the first
thing a plugin author does, and requiring an install before a syntax check
would be backwards.

---

### Milestone 8 — First-party plugins (core ecosystems)
**Goal:** Parity with the most common `uu` ecosystems.

Priority order:
1. [x] `cmake-cpp`
2. [x] `make-generic`
3. [x] `cargo-rust`
4. [x] `node` (workspace-aware `dev`)
5. [x] `go`
6. [x] `python-uv`

Each plugin ships `plugin.kpl` + fixture tests. No plugin merges without
`kap plugin test` passing.

**Exit criteria:** `kap plugin install --bundle core` enables build/test in
six fixture project types. ✅ — 42 fixture cases across the six, all green
without cmake, cargo, npm, go, or uv installed on the machine running them.

**Notes.**

*Detection priorities are all distinct*, so no pair of bundled plugins can ever
reach §3.2's tie error: `make-generic` 10, `cmake-cpp` 30, `node` 35,
`python-uv` 38, `cargo-rust` 40, `go` 45. `make-generic` sits below everything
on purpose — a Rust crate with a convenience Makefile should still be driven by
cargo.

*Two language problems were found by writing these plugins*, which is the
argument for writing real ones rather than more synthetic fixtures:

  * **`else if` never parsed.** The statement parser consumed the `if` with
    `match_text` and then recursed into `statement()`, which arrived with the
    keyword already eaten — so the condition parsed as an expression statement
    and the block's `{` was read as a record literal. The error surfaced as
    "expected ':' after record field" pointing at the first line of the else
    body. Fixed, with three regression tests.

  * **An enum member named `none` cannot be matched on.** §5.5's `pattern` rule
    lists `none` as a literal, so `none => ...` reads as the absent-value
    literal rather than as the member, and the exhaustiveness checker then
    reports the member as uncovered. Rather than change the grammar, the schema
    checker now refuses such a declaration and suggests a rename — caught even
    when the plugin has no `match` over the field yet, so it cannot lie in wait
    for whoever adds one. (`python-uv` calls its member `off`.)

*Where a plugin cannot know, it asks rather than guesses.* `cmake-cpp`'s `run`
needs a `run_target` because CMake has no notion of "the" binary; its `fmt` and
`lint` do nothing until `format_glob` is set, because a CMake tree routinely
contains vendored sources that must not be reformatted. `node` picks its
package manager from the lockfile, which is the only signal that describes the
repository rather than the machine.

---

### Milestone 9 — Bundled system plugins
**Goal:** `doctor` and `ports` in KPL.

- [x] `doctor` — checks `[requires]` from all matched plugins
- [x] `ports` — `ss` / `lsof` / `netstat` via step (see the note below on
      `/proc/net/tcp`)
- [x] `make-generic` fallback plugin — shipped in Milestone 8. (This milestone
      and §6.6 originally spelled it `generic-makefile` while Milestone 8's
      list spelled it `make-generic`; the Milestone-8 spelling is what ships.)

**Exit criteria:** `kap doctor` and `kap ports` work; implemented entirely
in KPL. ✅ — 10 fixture cases across the two, plus 17 end-to-end assertions
against the real binary.

**Notes.**

Both plugins are `composable: true` with a `detect` block that matches every
directory, so they ride alongside whichever plugin owns the project (§3.3) and
never compete to answer `kap build`.

*How `doctor` learns what to check.* KPL cannot see other plugins — that is a
deliberate sandbox property (§7), since a plugin that could enumerate its
neighbours could fingerprint the machine. So the core does the one thing only it
can: it reads every matched plugin's `requires` block and injects the result
into the doctor plugin's `config` record, through fields the plugin declares in
its own schema. Policy — what to check, what counts as healthy, what to print,
what exit status to produce — stays in KPL. This is the agreed resolution of
§4's "not hardcoded C++" requirement.

`any_of` grouping is preserved by comma-joining each group into one element,
which the plugin splits with the `split` builtin. Flattening it would report a
machine that has `ss` as missing `lsof` and `netstat`.

Injected values sit at the *bottom* of §5.12's chain, just above the schema
defaults, so "later wins" holds with no exception. Placing them higher — so a
project could not understate what doctor checks — buys nothing real: a committed
`kap.toml` is already trusted to run arbitrary shell through §5.13's hooks. What
it cost was the ability to experiment with the field at all.

*Why `ports` does not read `/proc/net/tcp`.* This milestone's wording offers
"`/proc/net/tcp` (Linux) or `lsof` fallback via step", and only the second half
is reachable from KPL — for two independent reasons, both of them features:

  1. §7's sandbox canonicalises every path passed to `project.read` and refuses
     anything outside the project root. `/proc` is outside every project root,
     and special-casing it would put a hole in the one rule that makes plugins
     safe to install from a git URL.
  2. `/proc/net/tcp` stores addresses and ports as big-endian hex, and KPL has
     no hex parsing, no integer formatting, and no bit operations —
     deliberately (§5.6). A plugin language that could decode it would be a much
     larger thing to audit.

*A composable match is not ownership.* Because `doctor` and `ports` claim every
directory, `kap detect` and `kap build` had to learn the difference: a
resolution with no *primary* is still "no plugin claims this directory", and
both now say so rather than reporting a sidecar as the owner.

---

### Milestone 10 — Polish + v1.0
**Goal:** Usable by early adopters.

- [x] `dev` concurrency polish (coloured prefixes, `-o` open-first-URL)
- [x] Shell completions (bash/zsh/fish)
- [x] `PLUGIN_API.md`, from this document's KPL section
- [x] `docker/dev-full.Dockerfile` with Rust, Node, Go, and Python/uv
- [x] Install script (`curl | sh` builds and installs the binary, the bundled
      plugins, and the registry index)
- [x] v1.0

**Exit criteria:** Install script works on Linux; docs cover plugin
authoring, config reference, and Docker workflow. ✅ — `scripts/install.sh`
verified end to end into a scratch prefix, and `./scripts/ci.sh` installs kap
and runs the result under `env -i` on every build.

**Notes.**

*Completion scripts are generated, not committed.* The command lists they
complete live in `core/main.cpp`, and a checked-in script is a second copy that
drifts the first time someone adds a subcommand. Completion is static — the
scripts never shell out to kap on Tab, which would make the shell feel broken
the first time kap was slow.

*`-o` costs the inherited terminal.* Opening the first URL means reading the
output, which means piping it, which means a tool checking `isatty()` drops its
colours. The trade is stated in the header and in the docs rather than hidden.

*`Options::open_url` is a seam, not tidiness.* Before it existed, running the
test suite opened real browser windows.

*`scripts/ci-full.sh` exists because `ci.sh` structurally cannot cover one
thing.* Golden-file tests prove a plugin emits the argv arrays it claims to;
they cannot prove those arrays are ones the real tools accept. A plugin can
emit `cargo buidl --release`, match its golden file perfectly, and be broken.
ci-full.sh creates a real project of each of the six kinds and runs kap against
it — 17 checks, all green against real toolchains.

*The install script runs the test suite before installing.* A binary that does
not pass its own tests should not land on someone's `PATH`.

---

## Roadmap status

All eleven milestones are complete, and `core/version.hpp` reads 1.0.0.

What a 1.x kap promises not to break: the §8 CLI surface, the §5 KPL language
(with `api_version` as the escape hatch for the day it must), the §5.4
CommandSpec contract that committed golden files are written against, and the
§6.4 on-disk layout.

Documentation, as the exit criteria require:

| Document | Covers |
|---|---|
| `docs/usage.md` | Every command, flag, exit code, and environment variable |
| `docs/configuration.md` | `kap.toml`, the four layers, hooks, the TOML subset |
| `docs/plugins.md` | Writing, testing, installing, and publishing a plugin |
| `docs/PLUGIN_API.md` | The KPL reference and grammar |
| `docs/dockerusage.md` | Both container images and both CI scripts |
| `kap-plugins/*/README.md` | Every configuration key of every bundled plugin |

---

### Post-v1 backlog (not blocking release)

- Windows port (executor + path semantics; KPL unchanged)
- `dev-full` image with all ecosystem toolchains
- Plugin signing / checksum policy hardening
- Numeric `for i in 0..N` in KPL if a real plugin needs it
- Optional `import` of shared KPL snippets (`.kpli`) with allow-list

---

## 12. Open Questions

1. **Windows executor** — still open, and still post-v1 as §11's backlog says.
   v1 targets Linux and macOS. `core/exec.hpp` is the whole surface that would
   need a `CreateProcess` backend; nothing above it — the language, the
   detection engine, the plugin manager — is POSIX-specific.
2. ~~**Plugin API versioning**~~ — **answered in Milestone 4**: a hard error
   wherever the user named the plugin explicitly (`kap plugin
   install/doctor/test`), and a warn-and-skip during detection, so one too-new
   plugin cannot break `kap build` in an unrelated project.
3. ~~**Registry trust**~~ — **answered in Milestone 7**: checksum per version
   in `index.toml`, over an in-tree SHA-256, and *enforced* rather than
   advisory. A signed index remains post-v1 (it needs key distribution a static
   git repository cannot provide).
4. ~~**`glob` implementation**~~ — **answered in Milestone 1**: a hand-written
   iterative matcher in `core/fs.hpp`, not `fnmatch(3)`. It is single-backtrack
   and therefore O(pattern × text) with no recursion, which matters because
   glob patterns come from plugins and §7 treats plugin input as untrusted — a
   pathological pattern must not be able to hang kap or blow its stack.
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
