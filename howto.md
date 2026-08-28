# howto.md — a contributor's guide to `kap`

This is the "how does any of this work, and how do I add to it" document.
It assumes you can read C++ but know nothing about this repository.

Companion documents:

| File | What it is for |
|---|---|
| `docs/design.md` | **The spec.** What we are building and why. The roadmap lives here. |
| `AGENTS.md` | The rules every change must follow. Short; read it once. |
| `docs/dockerusage.md` | The full Docker guide. |
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
Catch2. When we need a TOML parser or a test framework, we write a small one.
That is the point of the project, not an inconvenience to route around.

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
  version.hpp    version constants — the single source of truth (see §7)
  diag.hpp       errors: severity, source location, rendering
  argv.hpp       argv arrays and shell-quoting for display
  fs.hpp         sandboxed filesystem access (capped reads, globbing)
  toml.hpp/.cpp  the minimal TOML parser
  cli.hpp/.cpp   the command-line parser
  main.cpp       wiring: parse argv, dispatch, print, exit

tests/
  harness.hpp    the in-tree test framework (KAP_TEST, KAP_ASSERT*)
  test_main.cpp  the test runner's main()
  test_*.cpp     one file per module
  e2e.sh         end-to-end tests that drive the built binary
  fixtures/      sample input files

plugins/         first-party KPL plugins        (Milestone 2)
registry/        the plugin index               (Milestone 7)
docker/          dev container
scripts/         ci.sh, bootstrap.sh, in-docker.sh
docs/            design.md, dockerusage.md
```

`plugins/` contains the bundled `cmake-cpp` and `cargo-rust` examples used by
`kap plugin doctor`; `registry/` remains reserved for Milestone 7.

---

## 4. How the core works today

`kap` is being built in milestones (`docs/design.md` §11). Milestones 0 and 1
are done. So right now the binary does this and no more:

```
main()
  └─ cli::parse(argv)          -> an Invocation { command, argv, passthrough, global }
       └─ dispatch on command
            └─ "config" -> run_config
                            └─ "get" -> read <root>/kap.toml
                                          └─ fs::read_text     (capped, sandboxed)
                                          └─ toml::parse       (-> a Document)
                                          └─ Document::get     (dotted path lookup)
                                          └─ print the value
```

Everything else — detection, the KPL interpreter, the executor, the plugin
manager — is roadmap. When you add one, it slots into that same chain.

### 4.1 `core/diag.hpp` — how errors work

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
message without a location is much less useful, and users notice.

### 4.2 `core/fs.hpp` — the sandbox

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

### 4.3 `core/toml.hpp` / `toml.cpp` — the config parser

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

The parser holds a cursor (`pos_`, `line_`, `col_`) over a `string_view` and
walks it with `peek()` / `advance()`. The shape is: `parse_body` loops over
statements, `[header]` lines re-point which table subsequent keys land in, and
`parse_key_value` / `parse_value` handle the rest.

Construct values with `make_string()`, `make_integer()`, and friends — not with
designated-initialiser aggregates, which trip `-Wmissing-field-initializers`.

What is deliberately **not** supported: floats, datetimes, inline tables,
arrays of tables, quoted keys, and multi-line strings. Each is *rejected with a
diagnostic* rather than misparsed, so adding one later is safe. See the
Milestone 1 notes in the design doc.

### 4.4 `core/cli.hpp` / `cli.cpp` — the command line

One pass over the arguments producing an `Invocation`:

```
kap  --verbose  build  --root /tmp  --  --release
     └ global ┘ └cmd┘  └── global ──┘      └ passthrough ┘
```

- Global flags (`-n`/`--dry-run`, `--verbose`, `--root`, `--set`, `--help`,
  `--version`) may appear anywhere before `--`.
- The first positional is the **command**; later positionals are its own
  arguments (so `kap plugin install x` gives `command="plugin"`,
  `argv=["install","x"]`).
- Everything after `--` is **passthrough**, untouched, for the wrapped tool.
- `--root` and `--set` accept both `--root path` and `--root=path`.
- Anything else starting with `-` is an error. A lone `-` is a positional.

### 4.5 `core/argv.hpp` — argv arrays

`argv::List` is an ordered list of words with `quoted()`, which renders a
shell-safe display form for `--dry-run` output.

**`quoted()` is for display only.** The executor (Milestone 5) will pass argv
arrays straight to `posix_spawn`. `kap` never builds a shell string and never
calls `system()`, so the quoting is never parsed back — it exists so a human
can read what would have run.

---

## 5. Testing

Two layers, and you generally want both.

### 5.1 Unit tests — `tests/test_*.cpp`

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

Available assertions:

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

Some conventions worth keeping:

- **Name the test after the behaviour, not the function.** "keys after a table
  header land inside that table" beats "test_parse_body".
- **One behaviour per test**, so a failure names the problem.
- **Assert from both directions when fixing a bug.** The header-retargeting
  tests check that the key *is* under its section and *is not* at the root; the
  first alone would pass against several wrong implementations.
- **Test the failure paths.** Every parser needs cases for malformed input.

### 5.2 End-to-end tests — `tests/e2e.sh`

Unit tests cannot check exit codes, which stream a message went to, or how the
assembled binary dispatches arguments. `tests/e2e.sh` runs the real binary:

```bash
expect_stdout "config get reads a string" "localhost" \
    config get --root "$fixture" server.host
expect_status "an unknown command exits 2" 2 definitely-not-a-command
expect_stderr_contains "a missing key names it" "no key 'x'" config get x
```

Helpers available: `expect_status`, `expect_stdout`, `expect_stderr_contains`,
`expect_empty_stdout`. It is POSIX shell with no test framework, same rule as
everything else.

Run it directly while iterating:

```bash
./tests/e2e.sh ./build/kap 0.1.0
```

### 5.3 Sanitizers

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

### 5.4 Running everything

```bash
./scripts/ci.sh
```

Configure, build with `-Werror`, unit tests, e2e tests, `clang-format --Werror`,
and binary smoke tests. **Run it before you commit.** If it is green, CI will be
green.

---

## 6. Adding a feature, end to end

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
8. **Update the roadmap checkbox** in `docs/design.md`.
9. **Commit** (see §8).

### Where new code goes

- Reusable infrastructure -> a new `core/*.hpp` (header-only if it is small and
  has no state; add a `.cpp` and list it in `CMakeLists.txt` otherwise).
- A new command -> a `run_<name>` function in `main.cpp` plus a dispatch arm.
  When `main.cpp` gets long, that dispatch table moves into its own file.
- Anything a *plugin* should decide -> **not in C++ at all.** That is the whole
  architecture: if you find yourself writing `if (ecosystem == "rust")` in the
  core, stop and reread design doc §1.

---

## 7. Things that will bite you

**The version number lives in `core/version.hpp` and nowhere else.** CMake
parses the `kVersion*` constants out of that header to set `PROJECT_VERSION`,
and the tests assert the binary's banner against it. Bump the constants —
including `kVersionString` — and everything follows. Do not add a fourth copy.

**`-Werror` is on in CI, off locally.** `scripts/ci.sh` passes
`-DKAP_WERROR=ON`. A warning that you ignore locally will fail the pipeline.
This has already paid for itself twice: `-Wdangling-reference` caught a real
use-after-free in the test harness, and `-Wmissing-field-initializers` caught
six under-initialised structs.

**`clang-format` is enforced.** Version 18, matching the container. Run
`clang-format -i core/*.cpp core/*.hpp tests/*.cpp tests/*.hpp` before
committing, or let `./scripts/ci.sh` tell you.

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

## 8. Committing

From AGENTS.md §4, and taken seriously here:

- **One logical change per commit.** Do not bundle a bug fix with a
  refactor with a formatting pass. Formatting gets its own commit.
- **Verbose messages.** Say *what* changed and *why*. For a bug fix, describe
  the wrong behaviour, the consequence, and how you know it is fixed. Someone
  will read it in a year with no other context.
- **Commit often.** A big commit is harder to review and harder to revert.

The existing history is the style reference; `git log` is worth ten minutes.

---

## 9. The rules that are not negotiable

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

## 10. Quick reference

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
