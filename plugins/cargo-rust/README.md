# cargo-rust

Runs the right `cargo` invocation for a Rust crate or workspace.

## Detection

Claims any directory containing a `Cargo.toml`. Priority 40 — higher than
`cmake-cpp`, so a crate that vendors a CMake-built C dependency still resolves
to Cargo.

## Commands

| `kap` command | What it runs |
|---|---|
| `build` | `cargo build [--release]` |
| `check` | `cargo check` |
| `run`   | `cargo run [--release]` |
| `test`  | `cargo test` |
| `lint`  | `cargo clippy -- -D warnings` |
| `fmt`   | `cargo fmt`, or `cargo fmt -- --check` when `check = true` |
| `clean` | `cargo clean`, and reports the space freed |

Anything after `--` is appended to the underlying command:

```sh
kap test -- --nocapture
kap run  -- --port 8080
```

## Configuration

Under `[plugins.cargo-rust]` in `./kap.toml` or `~/.config/kap/config.toml`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `release` | bool | `false` | Adds `--release` to `build` and `run` |
| `check` | bool | `false` | Makes `fmt` verify instead of rewrite — what a CI job wants |

```toml
[plugins.cargo-rust]
release = true
```

Or, for one invocation only:

```sh
kap build --set release=true
```

## Tests

```sh
kap plugin test cargo-rust
```

See `howto.md` for the case-file format.
