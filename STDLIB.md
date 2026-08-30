# STDLIB.md

Every place kap replaced a package with the standard library, why, and what it cost.

The rules for C/C++ allow "libc, POSIX and the C++ standard library (libstdc++/libc++)" and nothing else — no Boost, no fmt, no abseil, no header-only drop-ins. kap holds to that. `ldd build/kap` reports libstdc++, libgcc_s, libc, libm and the loader; `deps-proof.txt` is the generated evidence, and `make deps-proof` reproduces it on your machine.

Nothing in this repository is vendored. There is no `vendor/`, no `third_party/`, no `.gitmodules`, and no `find_package`, `FetchContent`, or `ExternalProject` in `CMakeLists.txt`. Every line was written during the 72-hour window.

---

## First, the disclosure that matters most

**kap runs other people's programs. That is the product, not a hidden dependency — but it is a dependency, so here it is in full.**

The rules say invoking a separately installed tool "is a dependency you would be hiding" unless you "disclose in `STDLIB.md` and ensure graceful degradation if unavailable." kap is a task runner; running `cargo build` on your behalf is the entire point. What follows is every external program kap can start, when, and what happens when it is missing.

| Program | When | If absent |
|---|---|---|
| Your build tool (`cmake`, `cargo`, `npm`, `go`, `uv`, `mvn`, `zig`, …) | Only when you ask for a command that runs it | `kap doctor` reports it; the plugin's `requires` block declares it; the executor reports exit 127 with the name |
| `curl` **or** `wget` | Only `kap plugin install` from a URL | Named refusal: "neither curl nor wget is installed" |
| `git` | Only `kap plugin install` from a git URL | The install fails with the reason; the script and local-path routes still work |
| `sh` | Only to run a hook string *you* put in `kap.toml` | No hooks, no `sh` |

**Nothing in that table is linked, and none of it is needed for kap's own logic.** Detection, configuration merging, the plugin language, the type checker, `kap detect`, `kap doctor`, `kap plugin test`, and every `--dry-run` work on a machine with none of it installed. That is not a happy accident — plugins may only *declare* steps, and `core/exec.cpp` is the only place in the codebase that can create a process. It is why kap's own CI verifies all ten plugins with no cmake, cargo, npm, Go, Java, or Zig anywhere on the machine.

The honest cost is stated in `core/registry.cpp`: kap has **no HTTP client of its own**, because writing one would mean either plaintext-only downloads or a linked TLS library, and the rules permit neither. Shelling out to `curl` is the disclosed trade.

---

## The substitutions

Sixteen, each with what it replaced and what it cost.

### 1. TOML parser — `core/toml.cpp` (731 lines)

**Replaces:** [toml++](https://github.com/marzer/tomlplusplus) (~10k stars), cpptoml.

`kap.toml` and `registry/index.toml` are TOML, so a parser was unavoidable. Supports tables, dotted keys, arrays, inline tables, strings with escapes, integers, booleans, and comments — with located errors, because a config parser whose failure mode is "invalid TOML" is useless.

**Cost:** No dates, no floats, no multi-line literal strings. Nothing in kap's schema needs them, and adding them for completeness would be code no test exercised. Errors carry line and column, which toml++ also does; getting that right was most of the work.

### 2. JSON parser and serializer — `core/json.cpp` (466 lines)

**Replaces:** [nlohmann/json](https://github.com/nlohmann/json) (~44k stars), RapidJSON.

Used by `.kap/cache.json`, the plugin lockfile, and the `CommandSpec` golden-file format that plugin tests diff against. Parses and writes with stable key ordering — necessary, because two specs that behave identically must serialize identically or every golden file becomes flaky.

**Cost:** Numbers are `int64` only; no floating point. kap has no fractional quantity to store. Parsing is recursive descent with an explicit depth cap, so a hostile file cannot blow the stack.

### 3. SHA-256 — `core/sha256.hpp` (227 lines)

**Replaces:** OpenSSL, libsodium, picosha2.

Plugin payload integrity: `kap plugin install` enforces a `sha256:` digest from the registry index, and refuses a mismatch rather than warning. FIPS 180-4 from the specification, with the published test vectors in the unit suite.

**Cost:** Not constant-time, and not hardware-accelerated. Both are fine here: this verifies a download's integrity, it is not comparing a secret. Saying so in the header is part of the implementation.

### 4. Argument parser — `core/argv.hpp` + `core/cli.cpp` (308 lines)

**Replaces:** [CLI11](https://github.com/CLIUtils/CLI11), cxxopts, argparse.

Global flags anywhere before `--`, subcommands, and the `--` boundary that separates kap's arguments from the wrapped tool's (`kap build -- --release`). That boundary is the one thing a generic parser makes awkward and kap needs constantly.

**Cost:** No auto-generated help — `core/help.cpp` writes each page by hand, which is more text but reads better than any generator's output.

### 5. Test framework — `tests/harness.hpp` (271 lines)

**Replaces:** [Catch2](https://github.com/catchorg/Catch2) (~19k stars), GoogleTest, doctest.

A self-registering `KAP_TEST` macro, `KAP_ASSERT`, `KAP_ASSERT_EQ` that prints both operands, and `KAP_ASSERT_THROWS`. Runs **412 unit tests**. The harness has tests of its own — asserting that a failing assertion fails, and that its message carries file, line, and expression text.

**Cost:** No fixtures, no parameterised tests, no tags, no sharding. This is the exception the rules explicitly allow for a dev-only test dependency, and kap did not take it.

### 6. An embedded scripting language — `core/kpl.cpp` (2,906 lines)

**Replaces:** Lua (embedding a VM), or the more common alternative of a YAML config plus a template engine.

This is the largest substitution and the reason kap exists. **KPL** is a complete language: lexer, recursive-descent parser, a type checker that runs at load time, a tree-walking interpreter, and a sandboxed host object. Every plugin is written in it, and the binary knows nothing about any ecosystem without one.

**Cost:** No user-defined functions and no recursion, which the `node` plugin pays for by repeating its package-manager `match` in every command. That is a deliberate trade: an interpreter small enough to audit is worth more than an expressive one, and a plugin is a description rather than a program. Exhaustiveness is checked on `match`, so adding an enum member and forgetting an arm is a load-time error rather than a run-time surprise.

### 7. Binary serialization — `core/kapc.cpp` (542 lines)

**Replaces:** Protocol Buffers, Cap'n Proto, cereal, MessagePack.

The AST cache: parsing every plugin on every invocation is wasted work, so a compiled AST is written once and memory-mapped back. A tagged binary format with a magic number, a format version, and length-prefixed fields.

**Cost:** Not portable across architectures, and it does not try to be — it is a cache, keyed by source mtime, discarded when it does not match. The format version exists because this bit me: adding a statement kind to KPL renumbered an enum serialized by value, and stale caches decoded as the wrong kind. `kFormatVersion` is now bumped whenever the shape changes.

### 8. Process execution — `core/exec.cpp` (845 lines)

**Replaces:** [boost::process](https://www.boost.org/doc/libs/release/doc/html/process.html), reproc, subprocess libraries.

`fork` + `execvp` directly, with pipes, `poll`-based multiplexing for concurrent steps with labelled interleaved output, signal handling, and exit-status translation including `128+n` for a signalled child.

**Cost:** POSIX only — no Windows. `fork`+`execvp` rather than `posix_spawn` specifically because a step may set its own working directory, and `posix_spawn_file_actions_addchdir_np` is a glibc extension musl lacks; between `fork` and `exec`, `chdir(2)` is one portable line. Notably kap **never calls `system()`** and never builds a shell string: every step is an argv array, which is why `--dry-run` can show you exactly what will run and why there is no quoting bug to have.

### 9. Filesystem sandbox and globbing — `core/fs.hpp` (254 lines)

**Replaces:** `glob(3)` wrappers, Boost.Filesystem, path-traversal guards from any web framework.

`std::filesystem` gives path manipulation but no globbing and no containment check. Both were needed: plugins may read files, and a plugin must not be able to read `../../etc/passwd`. Paths are canonicalised with `weakly_canonical`, which resolves both `..` and symlinks, and anything escaping the project root is refused rather than clamped.

**Cost:** Globbing expands a wildcard in the final path component only — no `**`. Results are capped at 10,000 and sorted, so a plugin's step list is deterministic.

### 10. Terminal styling — `core/style.hpp` (110 lines)

**Replaces:** termcolor, fmt's color support, ncurses for capability detection.

ANSI escapes with two gates: `NO_COLOR` (no-color.org, any value counts) and `isatty`. Asked of **stderr** for diagnostics and **stdout** for the executor, because `kap build > log` should still colour the error you are about to read.

**Cost:** No terminfo, so no support for terminals that need something other than ANSI. Unicode marks (`✓ ✗ ·`) fall back to ASCII by inspecting `LC_ALL`/`LC_CTYPE`/`LANG` for "UTF", verified with `LC_ALL=C`.

### 11. Non-cryptographic hashing — `core/hash.hpp` (60 lines)

**Replaces:** xxHash, absl::Hash, CityHash.

FNV-1a, eight lines of actual algorithm, used for cache keys. Identical output on every platform, which matters because a cache key that differs between builds is a cache that never hits.

**Cost:** Weaker distribution than xxHash and slower on long inputs. Irrelevant for keys of a few hundred bytes.

### 12. Shell completion generation — `core/completions.cpp` (230 lines)

**Replaces:** clap_complete (Rust's), or hand-maintained completion files that drift.

`kap completions bash|zsh|fish` emits a script generated from the same command table that drives dispatch, so completions cannot describe a command that does not exist. CI syntax-checks all three.

**Cost:** Three shells; no PowerShell, no Nushell.

### 13. XDG base directories — `core/paths.hpp` (183 lines)

**Replaces:** platformdirs, xdg, whereami.

`XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` with the specified defaults, plus resolving the running executable's own path to find plugins installed beside it. Pointing all three at a scratch directory gives a completely isolated kap, which is how the test suite stays hermetic.

**Cost:** Linux and macOS conventions only.

### 14. Diagnostics — `core/diag.hpp` (164 lines)

**Replaces:** fmt for formatting, plus the diagnostic machinery you would lift from a compiler frontend.

Severity, source location, message, and notes, rendered in the clang/rustc shape. Every KPL parse and type error carries `file:line:column`, which is the difference between a plugin language people can debug and one they cannot.

**Cost:** No source-snippet rendering with carets under the offending span. Location text only.

### 15. Version and lockfile handling — `core/registry.cpp` (1,419 lines)

**Replaces:** a semver crate, plus the install pipeline of any package manager.

Version comparison, a lockfile recording what was installed and from where, checksum enforcement, staged installs that validate before writing, and a rollback path if validation fails.

**Cost:** Version comparison is simpler than full semver — no pre-release precedence rules, no build metadata. kap pins exact versions, so range resolution never arises.

### 16. Directory walking with an explicit budget — `core/fs.hpp`

**Replaces:** the recursive-walk helpers people import Boost for.

Every traversal has a cap. A read is capped at 1 MiB, a glob at 10,000 entries, and a walk-up at a configured depth. A tool that can be pointed at any directory on your disk should not be able to hang on one.

**Cost:** A project genuinely exceeding a cap is refused rather than served slowly. Stated in the sandbox table in `docs/PLUGIN_API.md` rather than discovered.

---

## What the standard library made painful

**No string formatting.** `std::format` is C++20 on paper and absent from the libstdc++ that ships with the compilers this had to build on, so every message is `operator<<` or string concatenation. This is why KPL grew a `pad(s, width)` builtin: aligning a column in the `doctor` plugin needed padding, and there was no `{:<18}` to reach for. The builtin counts UTF-8 code points rather than bytes, because padding by encoded length misaligns exactly the rows you added it for.

**No `std::filesystem` globbing.** Path manipulation is thorough; matching a pattern against a directory is absent entirely.

**No process API.** `std::system` exists and is unusable — it takes a shell string, which means quoting bugs and no way to set a per-child working directory. Everything worth having is raw POSIX.

**No `std::string::split`.** Written by hand more than once before it became a helper.

**Errors as exceptions or as values, never both.** Deciding per-layer took real thought: `diag::Error` is thrown for programmer-facing failures with a location, while a failing child process returns an `Outcome`, because a tool exiting 1 is an ordinary result rather than an exceptional one.

---

## The package this makes unnecessary

Not one package — **the class of them.** A tool like this normally imports a TOML parser, a JSON parser, an argument parser, a test framework, a process library, a logging library, a terminal-colour library, and an embedded scripting runtime. Eight ecosystems of transitive dependencies for a binary whose actual job is to run `cmake --build`.

kap is 15,397 lines of C++ that links libc and libstdc++, ships a programming language, and is verified by 412 unit tests, 256 end-to-end tests, and 74 plugin golden cases — none of which requires a single ecosystem toolchain to be installed.

The nearest comparison in spirit is `just` or `task`, both of which are excellent and both of which carry substantial dependency trees. The interesting claim is not that kap is smaller. It is that the ecosystem knowledge lives in **1,487 lines of plugin text** rather than in the binary, so adding Elixir support is a file someone writes in two minutes and never a release of kap.

---

## Verifying all of this

```sh
make              # one command, produces ./build/kap
make deps-proof   # regenerates deps-proof.txt on your machine
make test         # 412 unit + 256 e2e + 74 golden cases
ldd build/kap     # libstdc++, libgcc_s, libc, libm, loader
```
