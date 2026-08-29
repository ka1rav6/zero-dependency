# kap documentation

**kap** = "know project, act". It works out what kind of project you are
standing in and runs the right underlying tool.

```console
$ cd some-cmake-project && kap build
$ cd some-rust-crate    && kap build
$ cd some-monorepo      && kap dev
```

The binary knows nothing about CMake, Cargo, or npm. All of that lives in
**plugins** written in KPL, a small language the core interprets — so adding an
ecosystem means writing one text file, not rebuilding kap.

---

## Start here

| If you want to… | Read |
|---|---|
| Install kap and start using it | **[usage.md](usage.md)** |
| Look up one command, exhaustively | **[commands.md](commands.md)** |
| Change what a plugin does for your project | **[configuration.md](configuration.md)** |
| Write a plugin for an ecosystem kap does not know | **[plugins.md](plugins.md)** |
| Look up a KPL keyword, builtin, or type | **[PLUGIN_API.md](PLUGIN_API.md)** |
| Work on kap itself | **[../howto.md](../howto.md)** |
| Develop in the pinned container | **[dockerusage.md](dockerusage.md)** |
| Understand why any of it is shaped this way | **[design.md](design.md)** |

Per-plugin documentation — every configuration key, every command — lives in
each plugin's own README, next to the plugin:
[cmake-cpp](../kap-plugins/cmake-cpp/README.md) ·
[cargo-rust](../kap-plugins/cargo-rust/README.md) ·
[node](../kap-plugins/node/README.md) ·
[go](../kap-plugins/go/README.md) ·
[python-uv](../kap-plugins/python-uv/README.md) ·
[make-generic](../kap-plugins/make-generic/README.md) ·
[doctor](../kap-plugins/doctor/README.md) ·
[ports](../kap-plugins/ports/README.md)

---

## The two ideas

**Nothing is hardcoded.** `kap build` in a Rust crate runs `cargo build`
because a plugin says so, not because the binary contains the word "cargo".
Detection rules, command recipes, and configuration schemas are all declared in
`plugin.kpl` files. `kap detect` will show you which plugin claimed a directory
and why; `kap build -n` will show you the exact commands before any of them run.

**Zero dependencies.** kap links against the C++ standard library and POSIX,
and nothing else — no TOML library, no JSON library, no CLI parser, no test
framework, no crypto library. Each of those is a small in-tree implementation.
That is the project, not a constraint it works around.

---

## Ninety seconds

```console
$ curl -fsSL https://raw.githubusercontent.com/ka1rav6/zero-dependency/main/scripts/install.sh | sh

$ cd ~/code/my-project

$ kap detect                  # who owns this directory, and why
cmake-cpp  priority=30 score=1
  markers: CMakeLists.txt
  source: bundled (/usr/local/share/kap/plugins/cmake-cpp)
  root:   /home/you/code/my-project
  cache:  miss (rescanned)

$ kap doctor                  # do I have what it needs
kap doctor
  plugin   cmake-cpp
  ok       cmake
  ok       ninja  (optional)
healthy

$ kap build -n                # what would happen
$ mkdir -p build
$ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
$ cmake --build build

$ kap build                   # do it
```

Every command has a page in the terminal too:

```console
$ kap help                    # the index
$ kap install --help          # one command
$ kap help plugin install     # subcommands too
```
