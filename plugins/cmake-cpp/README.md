# cmake-cpp

Runs the right `cmake` invocation for a CMake project.

## Detection

Claims any directory containing a `CMakeLists.txt`. Priority 30, so a plugin
that recognises something more specific (say a Rust crate that also happens to
ship a CMake shim) wins.

## Commands

| `kap` command | What it runs |
|---|---|
| `build` | `mkdir -p <build_dir>`, then `cmake -S . -B <build_dir> [-G ...] -DCMAKE_BUILD_TYPE=...`, then `cmake --build <build_dir>` |
| `check` | The configure step only — the cheapest thing that catches a broken `CMakeLists.txt`, a missing dependency, or an absent toolchain |
| `test`  | `ctest --test-dir <build_dir> --output-on-failure` |
| `run`   | `<build_dir>/<run_target>` — CMake cannot know which binary you meant, so `run_target` is required rather than guessed |
| `install` | `cmake --install <build_dir> [--prefix <install_prefix>]` |
| `fmt`   | `clang-format -i` over `format_glob`; with `--set check=true`, `--dry-run --Werror` instead |
| `lint`  | `clang-tidy -p <build_dir>` over `format_glob` |
| `clean` | `rm -rf <build_dir>`, and reports the space freed |

`fmt` and `lint` do nothing until you set `format_glob`. That is deliberate: a
CMake tree routinely contains vendored sources and generated output, and
reformatting those would be worse than doing nothing.

Anything after `--` is appended to the *build* step, not the configure step:

```sh
kap build -- --target kap_tests
```

## Configuration

Set these in `./kap.toml` (committed, per project) or `~/.config/kap/config.toml`
(yours, everywhere), under `[plugins.cmake-cpp]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `generator` | `auto` \| `ninja` \| `make` \| `unix_makefiles` | `auto` | `auto` passes `-G Ninja` when `ninja` is on PATH and otherwise lets CMake choose |
| `build_dir` | string | `build` | Where to configure and build |
| `build_type` | string | `Debug` | Value for `-DCMAKE_BUILD_TYPE` |
| `cmake_args` | list of strings | `[]` | Extra flags appended to the configure step |
| `run_target` | string | `""` | What `kap run` executes, relative to `build_dir` |
| `install_prefix` | string | `""` | `--prefix` for `kap install`; empty leaves CMake's default |
| `format_glob` | string | `""` | Which files `fmt` and `lint` look at, e.g. `src/*.cpp` |
| `check` | bool | `false` | `kap fmt --set check=true` verifies instead of rewriting |

```toml
[plugins.cmake-cpp]
generator  = "ninja"
build_dir  = "out"
build_type = "RelWithDebInfo"
cmake_args = ["-DBUILD_TESTING=OFF"]
```

## Tests

```sh
kap plugin test cmake-cpp
```

Cases live in `tests/`: a fixture project tree under `tests/fixtures/`, and one
golden `CommandSpec` per case under `tests/expected/`. Nothing is executed —
the command block is evaluated and the resulting step list is compared with the
golden file. See `howto.md` for the case-file format.
