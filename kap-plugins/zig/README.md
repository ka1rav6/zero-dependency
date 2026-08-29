# zig

Build, test, run, and format Zig projects.

Zig's build system is itself a Zig program: `build.zig` declares named steps,
and `zig build <step>` runs one. Nothing outside that file knows which steps a
project has — so this plugin reads it before running `test`, `run`, or `check`,
and tells you plainly when the step you asked for is not declared.

## Detection

Claims a directory containing `build.zig` or `build.zig.zon`. Priority 42, so
it out-ranks `cmake-cpp` (30) in a project that carries both — a Zig project
with a CMake shim is still a Zig project.

`build.zig.zon` is listed as well because a package consumed only as a
dependency may ship the manifest without a build script of its own.

## Commands

| `kap` command | What it runs |
|---|---|
| `build` | `zig build [<build_step>] [-Doptimize=...]` |
| `test` | `zig build <test_step>` — default step `test` |
| `run` | `zig build <run_step>` — default step `run` |
| `check` | `zig build <check_step>` if declared, else `zig ast-check` over `source_glob` |
| `lint` | `zig ast-check` over `source_glob` |
| `fmt` | `zig fmt .`, or `zig fmt --check .` when `check = true` |
| `install` | `zig build install [--prefix ...]` |
| `clean` | `rm -rf zig-out .zig-cache zig-cache`, and reports the space freed |

### Missing steps

`test` and `run` read `build.zig` first:

```console
$ kap test
kap: error: build.zig declares no "test" step, which is what 'kap test' runs
```

Nothing is spawned. Without the check, the failure would surface as a Zig
compile error raised inside the build script — accurate, but it buries the one
fact that matters. Point the command at a step you do have with
`--set test_step=unit`, or add the step to `build.zig`.

`check` does not fail when its step is missing. A `check` step is the community
convention (it is what ZLS documents for editor diagnostics) but far from
universal, so without one the plugin falls back to `zig ast-check` on every file
matching `source_glob`. That is less thorough than a real semantic pass — it
parses and checks the AST rather than compiling — but it needs nothing from the
project and still catches a file that cannot compile.

### `clean` and the cache rename

Zig renamed `zig-cache` to `.zig-cache` in 0.12. Both are in `clean_paths`, so
the plugin works either side of that change without asking which Zig you have.
`rm -rf` on a path that does not exist is not an error.

## Configuration

Under `[plugins.zig]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `optimize` | `debug` \| `release_safe` \| `release_fast` \| `release_small` | `debug` | Maps to `-Doptimize=`; `debug` passes no flag, since that is already `zig build`'s default |
| `build_step` | string | `""` | Step for `kap build`; empty means Zig's default (install) step |
| `run_step` | string | `run` | Step for `kap run` |
| `test_step` | string | `test` | Step for `kap test` |
| `check_step` | string | `check` | Step for `kap check`, when `build.zig` declares it |
| `source_glob` | string | `src/*.zig` | What `lint` and the `check` fallback look at |
| `prefix` | string | `""` | `--prefix` for `kap install`; empty leaves Zig's default (`zig-out`) |
| `check` | bool | `false` | `kap fmt --set check=true` verifies instead of rewriting |
| `clean_paths` | list of strings | `["zig-out", ".zig-cache", "zig-cache"]` | What `kap clean` removes |

```toml
[plugins.zig]
optimize    = "release_fast"
source_glob = "src/**.zig"
```

Unlike `gofmt`, `zig fmt --check` exits non-zero when a file is misformatted, so
`kap fmt --set check=true` works in CI without inspecting its output.

## Tests

```sh
kap plugin test zig
```

The `no-steps` fixture is a `build.zig` with an install artifact and nothing
else, which is what covers both the `fail` path and the `ast-check` fallback.
