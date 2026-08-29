# make-generic

Drives any project that has a Makefile.

## Detection

Claims a directory containing `GNUmakefile`, `makefile`, or `Makefile` — the
three names GNU make itself looks for, in its own order.

Priority **10**, deliberately below every ecosystem plugin. A Rust crate with a
convenience Makefile still resolves to `cargo-rust`; a CMake project with a
generated Makefile still resolves to `cmake-cpp`. This plugin is the answer
when nothing more specific applies.

## Commands

Every command is `make <target>`. Which target is configuration, because
Makefile conventions genuinely vary — plenty of projects call their test target
`check` rather than `test`.

| `kap` command | Target (default) |
|---|---|
| `build` | `all` |
| `test` | `test` |
| `check` | `check` |
| `clean` | `clean` — and reports the space freed |
| `lint` | `lint` |
| `fmt` | `fmt` |
| `run` | `run` |
| `dev` | `dev` |
| `install` | `install` |
| `ci` | `ci` |

Arguments after `--` are appended, which is how you pass variables:

```sh
kap build -- V=1 PREFIX=/opt
```

If your Makefile has no such target, `make` says so. kap does not try to read
your Makefile and second-guess it.

## Configuration

Under `[plugins.make-generic]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `makefile` | string | `""` | `-f <file>`; empty lets make find it |
| `jobs` | string | `""` | `-j <n>` for `build` and `ci`; empty omits it |
| `build_target` … `ci_target` | string | see the table above | The target each command runs |
| `make_args` | list of strings | `[]` | Flags added to every invocation |

`jobs` is a string, not an integer, because KPL has no int-to-string
conversion and `-j 8` needs the `8` as a word.

```toml
[plugins.make-generic]
jobs = "8"
test_target = "check"
make_args = ["--warn-undefined-variables"]
```

## Tests

```sh
kap plugin test make-generic
```
