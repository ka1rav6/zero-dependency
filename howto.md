# howto.md — a contributor's guide to `kap`

This is the "how does any of this work, and how do I add to it" document.
It assumes you can read C++ but know nothing about this repository.

Companion documents:

| File | What it is for |
|---|---|
| `docs/design.md` | **The spec.** What we are building and why. The roadmap lives here. |
| `AGENTS.md` | The rules every change must follow. Short; read it once. |
| `docs/dockerusage.md` | The full Docker guide. |
| `plugins/*/README.md` | Per-plugin user documentation (config keys, commands). |
| `howto.md` (this file) | How the code actually works, and how to work on it. |

If this file and `docs/design.md` ever disagree, **the design doc wins** — and
please fix this file.

---

## 1. The thirty-second version

`kap` is a CLI that works out what kind of project you are standing in and runs
the right underlying tool:

```sh
cd some-rust-project && kap build     # runs: cargo build
cd some-cmake-project && kap build    # runs: cmake --build build
```

The interesting design decision is that **the binary knows nothing about Rust
or CMake**. All of that knowledge lives in plugins written in KPL (Kap Plugin
Language), a small DSL that the C++ core interprets. Adding support for a new
ecosystem means writing a `.kpl` file, not recompiling `kap`.

The second design decision is **zero dependencies**: the binary links against
the C++ standard library and POSIX, and nothing else. No toml++, no CLI11, no
nlohmann/json, no Catch2. When we need a TOML parser, a JSON parser, or a test
framework, we write a small one. That is the point of the project, not an
inconvenience to route around.

---

## 2. Getting set up

### The Docker path (recommended, and what CI uses)

```bash
docker compose run --rm dev
```

That drops you into a shell inside a container with a pinned toolchain. Your
working copy is bind-mounted at `/kap`, so edit files in your normal editor on
the host and build inside the container. Then:

```bash
./scripts/ci.sh
```

`docs/dockerusage.md` covers this properly.

### The local path

You need CMake ≥ 3.16, Ninja, a C++20 compiler, and `clang-format` (18, to
match CI):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Local builds are supported, but **CI is the source of truth**. If something
passes locally and fails in the container, the container is right.

---

## 3. What is in the repository

```
core/            the kap binary and its library
  version.hpp    version constants — the single source of truth (see §9)
  diag.hpp       errors: severity, source location, rendering
  argv.hpp       argv arrays and shell-quoting for display
  fs.hpp         sandboxed filesystem access (capped reads, globbing)
  hash.hpp       FNV-1a, for cache keys
  toml.hpp/.cpp  the minimal TOML parser (user config)
  json.hpp/.cpp  the minimal JSON parser/writer (golden files, caches)
  cli.hpp/.cpp   the command-line parser
  kpl.hpp/.cpp   KPL: lexer, parser, schema, type checker, interpreter, host
  kapc.hpp/.cpp  the .kapc AST cache
  plugin.hpp/.cpp plugin discovery and the fixture-test runner
  main.cpp       wiring: parse argv, dispatch, print, exit

tests/
  harness.hpp    the in-tree test framework (KAP_TEST, KAP_ASSERT*)
  test_main.cpp  the test runner's main()
  test_*.cpp     one file per module
  e2e.sh         end-to-end tests that drive the built binary
  fixtures/      sample input files

plugins/         first-party KPL plugins
  cmake-cpp/     plugin.kpl + README.md + tests/{fixtures,expected}
  cargo-rust/    same shape
registry/        the plugin index               (Milestone 7)
docker/          dev container
scripts/         ci.sh, bootstrap.sh, in-docker.sh
docs/            design.md, dockerusage.md
```

---

## 4. How the core works today

`kap` is being built in milestones (`docs/design.md` §11). Milestones 0–3 are
done: the shared infrastructure, the whole KPL front-end and interpreter, the
AST cache, and `kap plugin doctor` / `kap plugin test`. Detection, the
executor, config merging, and the plugin manager are next.

Two chains exist today. The Milestone-1 one:

```
main()
  └─ cli::parse(argv)          -> Invocation { command, argv, passthrough, global }
       └─ "config get <key>"
            └─ fs::read_text     (capped, sandboxed)
            └─ toml::parse       (-> a Document)
            └─ Document::get     (dotted path lookup)
```

And the Milestone-3 one, which is the interesting half:

```
main()
  └─ "plugin test [name]"
       └─ plugin::discover(root)      -> every <root>/plugins/*/plugin.kpl
            └─ kapc::load(source, cache)
                 ├─ valid .kapc?  -> kapc::decode        (skip the parser)
                 └─ otherwise     -> kpl::parse          (and write a .kapc)
            └─ kpl::type_check(plugin)                   (reject before running)
            └─ for each tests/expected/*.steps.json:
                 ├─ kpl::build_config(plugin, overrides) (schema defaults + overrides)
                 ├─ kpl::host_project(fixture_dir)       (the §7 sandbox)
                 ├─ kpl::evaluate(plugin, command, ...)  -> a CommandSpec
                 └─ compare kpl::to_json(actual) with the golden file
```

When you add detection or the executor, they slot into that same chain: detect
picks *which* plugin, the executor consumes the `CommandSpec` that
`kpl::evaluate` already produces.

---

## 5. The infrastructure modules

### 5.1 `core/diag.hpp` — how errors work

Every subsystem reports failures the same way, so the user sees one consistent
format and `main()` needs exactly one place to catch them.

```cpp
throw diag::Error{diag::error("expected a value", {"kap.toml", 2, 5})};
```

renders as:

```
kap: error: kap.toml:2:5: expected a value
```

Three pieces:

- **`Location`** — a file name plus an optional 1-based line and column. The
  file may be a synthetic name like `<argv>` when there is no real file. If
  there is no name at all, it renders as `<unknown>` so the line and column
  still survive.
- **`Diagnostic`** — severity, message, location, and follow-up notes.
- **`Error`** — a `std::exception` carrying a `Diagnostic`, with the report
  pre-rendered so `what()` is useful.

Build diagnostics with `diag::error()`, `diag::warning()`, or `diag::note()`
rather than filling a `Diagnostic` in by hand.

**When you write a new subsystem, throw `diag::Error` with a location.** A
message without a location is much less useful, and users notice. This is not
hypothetical: the interpreter shipped with its source name uninitialised, so
every run-time plugin error said `<unknown>:2:14` and told the author nothing
about *which* plugin was broken.

### 5.2 `core/fs.hpp` — the sandbox

Every filesystem access a plugin can eventually cause goes through here, so
that the limits in design doc §7 live in one auditable file:

- `read_text(path, max_bytes = 1 MiB)` — the cap is enforced **while reading
  the stream**, not by trusting `stat()`. That distinction is load-bearing:
  `/dev/zero` reports a size of zero and yields bytes forever.
- `glob(dir, pattern)` — non-recursive, sorted, capped at 10 000 results.
- `match_wildcard(pattern, text)` — `*` and `?`. Iterative, not recursive:
  patterns come from plugins, so the obvious recursive version's exponential
  blowup is a denial-of-service hole.
- `is_within(root, path)` — canonicalizes both and answers "is this really
  inside?". This is what stops `../../../../etc/passwd`.

**If you add a new way to touch the disk, add it here**, with its limit, and
test the limit.

### 5.3 `core/toml.hpp` / `toml.cpp` — the config parser

A hand-written recursive-descent parser for the subset of TOML we need:
tables (including dotted headers), bare keys, strings, integers, booleans, and
arrays of those. Parsed into a `Value` tree — a tagged union with a `Kind` —
and looked up by dotted path:

```cpp
const toml::Document doc = toml::parse(text, "kap.toml");
const std::optional<toml::Value> v = doc.get("plugins.cmake-cpp.generator");
```

A missing key is `std::nullopt`, not an error. A *syntax* error throws
`diag::Error` with the exact line and column.

Construct values with `make_string()`, `make_integer()`, and friends — not with
designated-initialiser aggregates, which trip `-Wmissing-field-initializers`.

What is deliberately **not** supported: floats, datetimes, inline tables,
arrays of tables, quoted keys, and multi-line strings.

### 5.4 `core/json.hpp` / `json.cpp` — golden files and caches

The same shape as the TOML parser, for the two places the design needs JSON:
the `CommandSpec` contract (§5.4), which `kap plugin test` compares against
golden files, and `.kap/cache.json` (§3.2), which Milestone 4 will write.

Two properties the callers depend on:

- **Output is canonical.** Objects are `std::map` and the writer emits keys
  sorted, so the same value always produces byte-identical text. That is what
  lets `plugin test` compare two specs as strings and print the two renderings
  as the diff.
- **Duplicate object keys are an error**, not last-wins. In a golden file a
  duplicate is always a mistake, and silently picking one hides it.

Floats and `\uXXXX` escapes are *rejected*, never approximated, so adding
either later cannot change the meaning of a file that parses today.

### 5.5 `core/cli.hpp` / `cli.cpp` — the command line

One pass over the arguments producing an `Invocation`:

```
kap  --verbose  build  --root /tmp  --  --release
     └ global ┘ └cmd┘  └── global ──┘      └ passthrough ┘
```

- Global flags (`-n`/`--dry-run`, `--verbose`, `--root`, `--set`, `--help`,
  `--version`) may appear anywhere before `--`.
- The first positional is the **command**; later positionals are its own
  arguments (so `kap plugin test cmake-cpp` gives `command="plugin"`,
  `argv=["test","cmake-cpp"]`).
- Everything after `--` is **passthrough**, untouched, for the wrapped tool.
- `--root` and `--set` accept both `--root path` and `--root=path`.
- Anything else starting with `-` is an error. A lone `-` is a positional.

### 5.6 `core/argv.hpp` — argv arrays

`argv::List` is an ordered list of words with `quoted()`, which renders a
shell-safe display form for `--dry-run` output.

**`quoted()` is for display only.** The executor (Milestone 5) will pass argv
arrays straight to `posix_spawn`. `kap` never builds a shell string and never
calls `system()`, so the quoting is never parsed back — it exists so a human
can read what would have run.

### 5.7 `core/hash.hpp` — cache keys

FNV-1a, eight lines, no tables, identical output on every platform. Used to
key `.kapc` files on a plugin's absolute path. **It is not a security
primitive.** If you ever find yourself using it for a trust decision, that is
the wrong function.

---

## 6. KPL, the plugin language

This is the heart of the project. `core/kpl.cpp` is about 1 800 lines and
contains five layers, in this order in the file:

| Layer | Entry point | What it does |
|---|---|---|
| Lexer | `kpl::lex` | text → located tokens |
| Parser | `kpl::parse` | tokens → `Plugin` AST |
| Schema | `kpl::schema`, `kpl::build_config` | `schema {}` → typed defaults, merged with overrides |
| Type checker | `kpl::type_check` | static errors, before anything runs |
| Interpreter | `kpl::evaluate` | one `command` block → a `CommandSpec` |

Plus `kpl::host_project`, the production `project` object, and
`kpl::to_json` / `kpl::spec_from_json`, the §5.4 wire format.

### 6.1 The five blocks of a plugin

```kpl
manifest { name = "cmake-cpp"  version = "1.0.0"  api_version = 1  priority = 30 }

detect   { file_exists "CMakeLists.txt" }

requires { any_of [cmake]  optional [ninja, make] }

schema   { build_dir: str = "build"
           generator: enum { auto, ninja } = auto }

command build(project, config, extra) { ... }
```

`manifest` and `schema` hold **statements** (`name = value`, `key: type =
default`). `detect` and `requires` hold **directives** — a name followed by
literal arguments — and are parsed by a separate grammar. That separation is
not cosmetic: parsing them as ordinary statements makes `file_exists "x"`
into two disconnected nodes, and makes `optional [ninja, make]` a hard parse
error, because `[` after an identifier is an index expression and an index
takes exactly one subscript.

A directive argument is deliberately never a bare identifier, so an identifier
at argument position unambiguously starts the next directive. Bare identifiers
*inside* a list are read as strings, which is why `any_of [cmake]` needs no
quotes.

### 6.2 Commands, values, and steps

A `command` block accumulates **steps** and returns nothing. The interpreter
builds a `CommandSpec`:

```cpp
struct Step { std::vector<std::string> command;   // argv, never a shell string
              std::optional<std::string> cwd;
              std::map<std::string, std::string> environment;
              std::optional<std::string> label; };

struct CommandSpec { std::vector<Step> steps; bool concurrent; bool report_freed_space; };
```

**Plugins never spawn processes.** `step` appends to the spec; only the C++
executor calls `posix_spawn`. That is what makes `--dry-run`, logging, and
sandboxing uniform rather than per-plugin.

A command may declare `project`, `config`, and `extra` as parameters, in any
combination — and it only sees the ones it declares. Anything else is a
load-time error naming the three that exist.

Value kinds are `none`, `str`, `int`, `bool`, `list<T>`, and `record`. There is
**no truthiness**: `if config.build_dir { ... }` is an error, not a
quietly-skipped branch.

### 6.3 The two step forms

```kpl
step mkdir "-p" dir                                    // variadic argv
step ["cmake", "--build", dir] + extra                 // a list expression
step { cmd: [pm, "run", "dev"], cwd: ws, label: ws }   // the record form
```

In the variadic form a string contributes one word and a list splices in all of
its words, so `step "ninja" dir` and `step ["ninja", dir]` produce the same
argv.

**The bare-word rule.** In `step mkdir "-p" dir`, `mkdir` is a program name and
`dir` is a variable — both are bare identifiers. The rule that makes both work:
*an identifier standing alone as a `step` argument resolves to a variable if
one is bound, and is otherwise the literal word.* It applies only in that
position — `step [name]` and `step name + ".txt"` still require a real binding
— so the one cost, a mistyped variable silently becoming a word, is confined to
where the syntax needs it.

The record form is the only way to set `cwd`, `env`, or `label`, which the
executor needs for `kap dev`'s concurrent prefixed output. Mixing it with other
arguments is an error, and an unrecognised field is rejected rather than
ignored — a dropped `cdw:` would run the step in the wrong directory with
nothing to go on.

### 6.4 Control flow

```kpl
if project.tool("ninja") { step ninja } else { step make }

for ws in project.glob("packages/*") {          // lists only; no numeric ranges
  if project.exists(ws + "/package.json") {
    step { cmd: ["npm", "run", "dev"], cwd: ws, label: ws }
  }
}

let generator = match config.generator {        // match is an EXPRESSION
  auto  => if project.tool("ninja") then "Ninja" else none,
  ninja => "Ninja",
}
```

Three things to know:

- **The `for` loop variable is scoped to the loop.** KPL is otherwise
  flat-scoped on purpose — an assignment inside an `if` body updates the outer
  variable, which is how `cmd = cmd + [...]` accumulates — but a loop variable
  leaking its last element has no use and is an easy way to write a plugin that
  works by accident.
- **A bare identifier in a `match` pattern is an enum member**, compared as its
  own text. `ninja => "Ninja"` never binds anything.
- **There is no catch-all pattern.** An uncovered value is a located run-time
  error. Over a schema `enum` the type checker proves coverage statically and
  names the members you forgot; over a plain `str` you must cover every value
  the plugin can produce.

### 6.5 The host surface

Everything KPL can learn about the world (design doc §5.8):

| Call | Type |
|---|---|
| `project.exists(path)` | `str -> bool` |
| `project.read(path)` | `str -> str` (1 MiB cap) |
| `project.glob(pattern)` | `str -> list<str>` (10 000 cap) |
| `project.tool(name)` | `str -> bool` (PATH scan, never execs) |
| `project.env(name)` | `str -> str?` (deny-listed) |
| `len(list)` | `list -> int` |
| `contains(hay, needle)` | `(str, str) -> bool` |
| `trim(s)` | `str -> str` |
| `split(s, sep)` | `(str, str) -> list<str>` |

Plus `project.root` and `project.matched_files`. There are no user-defined
functions in v1, so **that table is the complete list of things a plugin can
make the host do** — which is what keeps the sandbox auditable.

`kpl::Project` holds these as `std::function`, for two reasons: unit tests
drive the interpreter against a mocked project with no disk at all, and
`kap plugin test` swaps `tool` and `env` for declared values so a case cannot
depend on the machine it runs on. `kpl::host_project(root)` is the production
implementation, and it is where §7's sandbox lives:

- One choke point (`resolve_in_root`) canonicalises every plugin-supplied path
  and refuses anything outside the project root. `weakly_canonical` resolves
  both `..` and symlinks, so neither escape works. One door, so the guarantee
  is checkable.
- `project.env` applies a deny-list (`*_TOKEN`, `*_KEY`, `*_SECRET`,
  `*_PASSWORD`, `AWS_*`, ...) so a plugin cannot exfiltrate CI credentials
  through a step argument, while ordinary build variables stay readable.
- `project.tool` refuses names containing a slash, so it cannot be turned into
  an arbitrary-path probe.

An unset callback means the capability is absent. Queries degrade safely
(`false` / `none` / `[]`); `read` raises, because `""` is indistinguishable
from an empty file.

### 6.6 The type checker

`kpl::type_check` runs before anything is evaluated, and `kap plugin doctor`
reports what it finds. It is worth understanding because it is what makes a
broken plugin fail at load rather than mid-build.

It reads the plugin's `schema` block, so `config.<field>` has its **declared**
type rather than being opaque. That buys three things:

- a `config` key not in the schema is a load-time error naming the key;
- element types survive concatenation, so `["cmake"] + config.cmake_args`
  checks as `list<str>` all the way to the `step` that consumes it;
- `match` over an enum field is checked for exhaustiveness.

A plugin with **no** `schema` block declares no config surface, so its `config`
reads stay unchecked rather than becoming false "unknown key" errors.

`StaticType::Unknown` is the lattice's wildcard: it satisfies any expectation
and any expectation satisfies it. Use it when a type genuinely cannot be known
(`project.env` returns `str?`), never as a shortcut to silence a check.

### 6.7 `core/kapc.hpp` — the AST cache

`kap` parses a `plugin.kpl` on every invocation, so §5.14 asks for the AST to
be cached under `~/.cache/kap/ast/`. The blob is a little-endian byte encoding
of the AST, decoded by a bounds-checked linear scan.

Three rules the cache follows, and they are worth preserving if you touch it:

1. **It can never turn a working plugin into a broken one.** Missing, stale,
   corrupt, truncated, wrong-version, unwritable directory — every one of them
   falls back to parsing the source. Only a genuine error in `plugin.kpl`
   propagates. Entries are written to a temporary file and renamed, so a
   concurrent `kap` never reads a half-written one.
2. **It can never decode into a subtly wrong AST.** Magic number, format
   version, range-checked enum tags, length prefixes validated against the
   bytes that actually remain, a depth cap, and trailing bytes rejected.
3. **`kFormatVersion` must be bumped whenever the AST's shape changes.** If you
   add a field to `Expr`, `Statement`, `Command`, or `Plugin`, bump it in the
   same commit. Otherwise a cache written by the previous build decodes into
   something that is not what it says it is — the one failure mode a cache must
   never have.

---

## 7. Writing a plugin

A plugin is a directory with `plugin.kpl` and a `README.md`. Start from
`plugins/cargo-rust/` — it is the smaller of the two.

```
plugins/my-tool/
├── plugin.kpl
├── README.md
└── tests/
    ├── fixtures/simple/…            a project tree to evaluate against
    └── expected/simple.build.steps.json
```

### 7.1 The case file

One file under `expected/` is one test case. `kap plugin test` evaluates a
command block against the fixture directory and compares the resulting
`CommandSpec` with the file. **Nothing is executed** — that is why CI can test
the CMake plugin on a machine with no CMake.

The file name binds the case by convention, `<fixture>.<command>.steps.json`,
and the JSON may override either half:

```json
{
  "fixture": "simple",              // optional; else from the file name, else
                                    // the plugin's single fixture directory
  "command": "build",               // optional; else the last dotted component
  "config":  { "build_dir": "out" },   // overrides on the schema defaults
  "extra":   ["--release"],            // passthrough arguments
  "tools":   ["cmake", "ninja"],       // what project.tool() reports present
  "env":     { "CI": "true" },         // what project.env() returns

  "steps": [ { "cmd": ["cmake", "--build", "out"] } ],
  "concurrent": false,
  "report_freed_space": false
}
```

The override is what lets one command have several cases — `cmake-cpp` has an
"auto generator, Ninja present" case and a "Ninja absent" case that would
otherwise collide on one file name.

`tools` and `env` are **declared, not sampled**. A case that consulted the real
`PATH` would pass or fail depending on whose laptop ran it, which breaks §5.1's
promise that the same inputs always yield the same `CommandSpec`. Everything
else — `exists`, `read`, `glob` — comes from the fixture directory through the
normal sandbox, which is what a fixture is *for*: put a `package.json` in the
fixture and the plugin's `project.exists("package.json")` branch is genuinely
exercised.

Omitted step fields (`cwd`, `env`, `label`) and spec fields (`concurrent`,
`report_freed_space`) take their defaults, so a golden file only spells out
what it cares about.

### 7.2 The loop

```bash
./build/kap plugin doctor --root .        # parses, validates, type-checks
./build/kap plugin test my-tool --root .  # runs the cases
```

`doctor` first — a type error there explains a `test` failure that would
otherwise look mysterious. On a mismatch, `test` prints the expected and actual
specs in full; the fastest way to write a new golden file is to write the
`steps` you think you want, run it, and paste the "actual" block if it is
right.

Both are part of `./scripts/ci.sh`, so a plugin cannot merge with a failing
case.

---

## 8. Testing the core

Two layers, and you generally want both.

### 8.1 Unit tests — `tests/test_*.cpp`

The harness is `tests/harness.hpp`, around 250 lines. Write a test like this:

```cpp
KAP_TEST("descriptive sentence about the behaviour")
{
    const auto doc = kap::toml::parse("[server]\nhost = \"localhost\"\n");
    KAP_ASSERT_EQ(doc.get("server.host")->str, "localhost");
});
```

Note the closing `});` — `KAP_TEST` opens a lambda and a constructor call, and
your braces close them. It looks odd for about a day.

| Macro | Use |
|---|---|
| `KAP_ASSERT(cond)` | a boolean condition |
| `KAP_ASSERT_EQ(a, b)` | equality, via the values' own `operator==` |
| `KAP_ASSERT_NE(a, b)` | inequality |
| `KAP_ASSERT_THROWS(Type, expr)` | `expr` must throw `Type` |

Tests self-register: a `KAP_TEST` creates a static object whose constructor
adds it to a global registry before `main()` runs, so there is no list to
update. **But you must add a new `test_*.cpp` to `CMakeLists.txt`** — that is
the one manual step. (The runner fails loudly if the registry ends up empty,
which is the usual symptom of forgetting.)

Conventions worth keeping:

- **Name the test after the behaviour, not the function.** "keys after a table
  header land inside that table" beats "test_parse_body".
- **One behaviour per test**, so a failure names the problem.
- **Assert from both directions when fixing a bug.** The header-retargeting
  tests check that the key *is* under its section and *is not* at the root; the
  first alone would pass against several wrong implementations.
- **Test the failure paths.** Every parser needs cases for malformed input.
- **For KPL, assert on the resulting `CommandSpec`, not on the AST.** The AST
  is an implementation detail; the spec is the contract. The `.kapc` round-trip
  test compares two *evaluated specs* precisely because comparing a handful of
  AST fields would miss a dropped one.
- **Mock the `project` object** rather than touching the disk, unless the disk
  is the thing under test:
  ```cpp
  kap::kpl::Project project;
  project.tool = [](std::string_view n) { return n == "ninja"; };
  const auto spec = kap::kpl::evaluate(plugin, "build", project, config, extra);
  ```
  Assign fields rather than using designated initialisers — `Project` gains
  members over time and `-Wmissing-field-initializers` is an error in CI.

### 8.2 End-to-end tests — `tests/e2e.sh`

Unit tests cannot check exit codes, which stream a message went to, or how the
assembled binary dispatches arguments. `tests/e2e.sh` runs the real binary:

```bash
expect_stdout "config get reads a string" "localhost" \
    config get --root "$fixture" server.host
expect_status "an unknown command exits 2" 2 definitely-not-a-command
expect_stderr_contains "a missing key names it" "no key 'x'" config get x
```

Helpers: `expect_status`, `expect_stdout`, `expect_stdout_contains`,
`expect_stderr_contains`, `expect_empty_stdout`. POSIX shell, no test
framework, same rule as everything else.

```bash
./tests/e2e.sh ./build/kap 0.1.0
```

### 8.3 Sanitizers

Most of `kap` is parsers walking buffers by index — precisely the code where an
off-by-one reads out of bounds and still passes every test. Build with
AddressSanitizer and UBSan when you touch one:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKAP_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

It costs roughly 2x runtime, which is why it is not the default. The suite is
expected to be clean under it; if you make it fail, you found a real bug.

### 8.4 Running everything

```bash
./scripts/ci.sh
```

Configure, build with `-Werror`, unit tests, e2e tests, `clang-format --Werror`,
`kap plugin doctor`, `kap plugin test`, and binary smoke tests. **Run it before
you commit.** If it is green, CI will be green.

---

## 9. Adding a feature, end to end

Say you are adding `kap detect` (Milestone 4).

1. **Read the design doc section first** (§3 here). If what you want to do is
   not covered, *ask* before deciding — AGENTS.md §3. Design decisions are made
   in the doc, not in a commit.
2. **Write the header** with the types and the doc comment explaining *why*.
3. **Write the tests before or alongside the implementation.** Include the
   malformed and edge cases; that is where the bugs are.
4. **Implement it.** Throw `diag::Error` with a location on failure.
5. **Add the `test_*.cpp` to `CMakeLists.txt`.**
6. **Add e2e assertions** if the feature is visible from the command line.
7. **Run `./scripts/ci.sh`.**
8. **Update the roadmap checkbox** in `docs/design.md`, and this file if the
   architecture changed.
9. **Commit** (see §11).

### Where new code goes

- Reusable infrastructure → a new `core/*.hpp` (header-only if it is small and
  has no state; add a `.cpp` and list it in `CMakeLists.txt` otherwise).
- A new command → a `run_<name>` function in `main.cpp` plus a dispatch arm.
  When `main.cpp` gets long, that dispatch table moves into its own file.
- A new KPL builtin → add it to **both** `Evaluator::project_call` /
  `stdlib_call` and the type checker's signature tables, in the same commit.
  They are deliberately parallel: `kap plugin doctor` must catch at load time
  exactly what would fail at run time, and a builtin present in one but not the
  other breaks that.
- Anything a *plugin* should decide → **not in C++ at all.** That is the whole
  architecture: if you find yourself writing `if (ecosystem == "rust")` in the
  core, stop and reread design doc §1.

---

## 10. Things that will bite you

**The version number lives in `core/version.hpp` and nowhere else.** CMake
parses the `kVersion*` constants out of that header to set `PROJECT_VERSION`,
and the tests assert the binary's banner against it. Bump the constants —
including `kVersionString` — and everything follows. Do not add a fourth copy.

**`-Werror` is on in CI, off locally.** `scripts/ci.sh` passes
`-DKAP_WERROR=ON`. A warning you ignore locally will fail the pipeline. This
has paid for itself repeatedly: `-Wdangling-reference` caught a real
use-after-free in the test harness, `-Wmissing-field-initializers` caught six
under-initialised structs, and `-Wswitch` caught both places that needed
handling when a new `Expr::Kind` was added.

**`clang-format` is enforced.** Version 18, matching the container. Run
`clang-format -i core/*.cpp core/*.hpp tests/*.cpp tests/*.hpp` before
committing, or let `./scripts/ci.sh` tell you.

**Bump `kapc::kFormatVersion` when the AST changes.** See §6.7. A stale cache
that still decodes is worse than one that fails.

**Keep the type checker and the interpreter in agreement.** Where they disagree
about a rule — bare words in step position, loop-variable scoping, which
parameters are bound, what a builtin accepts — one of them is wrong, and the
symptom is a plugin that `doctor` calls healthy and then dies mid-run. Both
implementations of each rule carry a comment pointing at the other; keep them.

**`std::move` and then reuse is a real bug, not a style issue.** The parser
built its conditional node by mutating the object it had just moved into that
node's own child, so every `then`/`else` expression carried a moved-from token
— an empty file name and a meaningless line number. Build a fresh node.

**Holding pointers into a `Value`'s table is safe — but only because it is a
`std::map`.** `std::map` guarantees references to existing elements survive
insertion; `std::unordered_map` does not, and a `std::vector` certainly does
not. If you ever change that container type, the TOML parser's `current` table
pointer becomes a use-after-free. The comment at that site says so; keep it.

**Do not bind assertion operands by reference.** `KAP_ASSERT_EQ` copies its
operands deliberately. `const auto& x = doc.get("k")->str;` dangles: the
temporary `optional` dies at the end of the declaration and lifetime extension
does not reach through it.

**Diagnostics go to stderr, values go to stdout.** `kap config get x` must be
safe to pipe. There are e2e tests asserting this; do not break them.

---

## 11. Committing

From AGENTS.md §4, and taken seriously here:

- **One logical change per commit.** Do not bundle a bug fix with a refactor
  with a formatting pass. Formatting gets its own commit.
- **Verbose messages.** Say *what* changed and *why*. For a bug fix, describe
  the wrong behaviour, the consequence, and how you know it is fixed. Someone
  will read it in a year with no other context.
- **Commit often.** A big commit is harder to review and harder to revert.

The existing history is the style reference; `git log` is worth ten minutes.

---

## 12. The rules that are not negotiable

Repeated from AGENTS.md because they are the ones that get broken:

1. **No external dependencies.** Standard library and POSIX only. Before adding
   an `#include`, check it is one of those. If you need functionality a library
   would give you, write a minimal version in-tree — that is the project.
2. **The tree stays green.** Every commit builds and passes the full suite.
3. **Comment generously, and explain *why*.** A beginner should be able to read
   this codebase. Comments that restate the code are noise; comments that
   explain the reasoning are the point.
4. **Test every feature separately**, so a failure isolates the cause.
5. **Follow the design doc. Ask before deviating.**

---

## 13. Quick reference

```bash
# Set up and build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Everything CI runs
./scripts/ci.sh

# Just the unit tests, with names
./build/kap_tests

# Just the end-to-end tests
./tests/e2e.sh ./build/kap 0.1.0

# Plugins
./build/kap plugin doctor --root .
./build/kap plugin test --root .
./build/kap plugin test cmake-cpp --root .

# Format
clang-format -i core/*.cpp core/*.hpp tests/*.cpp tests/*.hpp

# Sanitizer build (use when touching a parser)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKAP_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

# In the container
docker compose run --rm dev ./scripts/ci.sh

# Try the binary
./build/kap --help
./build/kap config get --root tests/fixtures/config server.host
```

Welcome aboard.
