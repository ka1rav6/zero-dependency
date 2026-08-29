# Configuring kap

kap is meant to work with no configuration at all. This is for when the default
is not what you want — a different CMake generator, a build directory somewhere
else, a command to run before every build.

The rule that makes it predictable: **later layers win, always.**

```
plugin defaults          declared in the plugin's own schema block
  ↓
~/.config/kap/config.toml    yours, on this machine, everywhere
  ↓
./kap.toml                   this project's, committed with the code
  ↓
--set key=value              this one invocation
```

Nothing in kap breaks that ordering. To see where you have landed:

```console
$ kap config get plugins.cmake-cpp.generator
ninja
$ kap --verbose build -n     # says which files were read
```

---

## Contents

- [The two files](#the-two-files)
- [What a config file looks like](#what-a-config-file-looks-like)
- [`[plugins.<name>]` — per-plugin settings](#pluginsname--per-plugin-settings)
- [`[detect]` — which plugin owns the directory](#detect--which-plugin-owns-the-directory)
- [`[hooks]` — run something before or after](#hooks--run-something-before-or-after)
- [`--set` on the command line](#--set-on-the-command-line)
- [Reading and writing from the CLI](#reading-and-writing-from-the-cli)
- [How merging works](#how-merging-works)
- [The TOML kap understands](#the-toml-kap-understands)
- [Files kap writes](#files-kap-writes)

---

## The two files

| File | Who it is for | Commit it? |
|---|---|---|
| `~/.config/kap/config.toml` | You, on this machine | No — it is yours |
| `./kap.toml` | This project, for everyone working on it | Yes |

Both are optional. kap works in a directory with neither.

The split matters: "I prefer make over ninja" is a fact about you and belongs
in the global file. "This project builds into `out/`" is a fact about the
project and belongs in the repository, where it applies to everyone who clones
it.

---

## What a config file looks like

```toml
# kap.toml — committed alongside the code.

# Which plugin owns this directory, when it is ambiguous.
[detect]
ecosystem = "cmake-cpp"
max_walk_up = 2

# Settings for one plugin. The keys come from that plugin's schema; its README
# lists them.
[plugins.cmake-cpp]
generator  = "ninja"
build_dir  = "out"
build_type = "RelWithDebInfo"
cmake_args = ["-DBUILD_TESTING=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]

# Shell commands to run around kap commands.
[hooks]
pre_build  = "date +'build started at %T'"
post_test  = "notify-send 'tests finished'"
```

---

## `[plugins.<name>]` — per-plugin settings

This is where almost everything lives. The keys are whatever the plugin
declares in its `schema` block, and kap validates them **before** running any
of the plugin's code:

```console
$ kap build
kap: error: configuration for plugin 'cmake-cpp' is not usable
      note: unknown config key 'genrator'
```

A typo fails immediately and by name, rather than leaving you watching a build
that ignores your setting for no visible reason.

Wrong types are caught the same way, and an enum tells you what would have
worked:

```console
$ kap build --set generator=clown
kap: error: configuration for plugin 'cmake-cpp' is not usable
      note: config key 'generator' does not accept 'clown'; expected one of: auto, ninja, make, unix_makefiles
```

To find out what a plugin accepts, read its README — every bundled plugin has
one, listing every key, its type, its default, and what it means. The
[docs index](README.md#start-here) links all nine.

### Configuring a plugin you do not use

Harmless. A `[plugins.node]` section in a Rust project is simply never
consulted; kap only validates the section belonging to the plugin that actually
matched.

---

## `[detect]` — which plugin owns the directory

```toml
[detect]
# Pin the plugin, when more than one matches. This is also what kap tells you
# to write when it refuses to guess.
ecosystem = "cargo-rust"

# How far up the tree to look for a project. 0 (the default) means "this
# directory only"; 3 lets `kap build` work from deep inside a source tree,
# the way git finds .git.
max_walk_up = 3
```

`max_walk_up` is off by default because walking upward can surprise you: run
`kap build` in an unrelated directory inside a monorepo and you would build the
monorepo. Opt in per project, where you know the shape of the tree.

---

## `[hooks]` — run something before or after

```toml
[hooks]
pre_build  = "echo starting"
post_build = "./scripts/notify.sh built"
pre_test   = "docker compose up -d test-db"
post_test  = "docker compose down"
```

The key is `pre_<command>` or `post_<command>` for any project command.

Three things to know:

- **Hooks are shell strings**, run through `/bin/sh -c`. That is why
  `a && b` works in a hook and would not in a plugin step. It is also the one
  place kap runs a shell: plugin steps are argv arrays end to end, so a
  directory named `my project` reaches the tool intact and a plugin cannot
  smuggle `; rm -rf ~` through a filename.
- **A failing pre-hook stops the command.** A failing post-hook is reported and
  changes the exit status.
- **Post hooks only run on success.** A `post_test = "notify-send 'tests
  finished'"` firing after a *failed* run would be actively misleading.

Hooks run in the project root. `kap build -n` shows them without running them.

Because a committed `kap.toml` can run arbitrary shell through hooks, it is
code, and cloning a repository and running `kap build` trusts it exactly as
much as running its `make` would. That is the same trust model every build tool
has; it is stated here so it is not a surprise.

---

## `--set` on the command line

The highest-precedence layer, and the one that does not persist:

```sh
kap build --set build_dir=out
kap build --set release=true --set features=serde,tokio
kap fmt   --set check=true          # verify instead of rewrite
```

The command line has no types, so kap converts the text using the plugin's
schema:

| Schema type | What you write | What the plugin gets |
|---|---|---|
| `str` | `--set build_dir=out` | `"out"` |
| `int` | `--set jobs=8` | `8` |
| `bool` | `--set release=true` | `true` |
| `enum` | `--set generator=ninja` | `ninja`, checked against the members |
| `list<str>` | `--set cmake_args=-DA=1,-DB=2` | `["-DA=1", "-DB=2"]` |
| `list<int>` | `--set levels=1,2,3` | `[1, 2, 3]` |

Only the **first** `=` splits key from value, so `--set cmake_args=-DA=1`
gives you `-DA=1` and not `-DA`. List values split on commas — which means a
value that must itself contain a comma has to come from a config file instead.

---
tcgetattr
## Reading and writing from the CLI

```console
$ kap config get plugins.cmake-cpp.build_dir          # effective value
out
$ kap config get --global detect.max_walk_up          # just the global file
3
$ kap config get --project plugins.cmake-cpp.build_dir
out

$ kap config set plugins.cmake-cpp.build_dir out      # writes ./kap.toml
$ kap config set --global plugins.cmake-cpp.generator make
$ kap config edit                                     # $EDITOR on ./kap.toml
$ kap config edit --global
```

`set` infers the type from what you type: `true`/`false` become booleans, a
plain number becomes an integer, anything else stays a string. So
`kap config set detect.max_walk_up 3` stores the number, which is what you
meant.

`set` rewrites the whole file through kap's TOML writer. Values survive
exactly; **comments and layout do not.** If you have a carefully commented
`kap.toml`, use `kap config edit`.

`-n` works on `set`, and writes nothing:

```console
$ kap -n config set plugins.cmake-cpp.build_dir out
would set plugins.cmake-cpp.build_dir = out in /home/you/code/my-project/kap.toml
```

---

## How merging works

Merging is **deep for tables, replacing for everything else**.

```toml
# ~/.config/kap/config.toml
[plugins.cmake-cpp]
generator  = "ninja"
cmake_args = ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
```

```toml
# ./kap.toml
[plugins.cmake-cpp]
build_dir  = "out"
cmake_args = ["-DBUILD_TESTING=OFF"]
```

The result:

```toml
generator  = "ninja"                    # from global; the project said nothing
build_dir  = "out"                      # from the project
cmake_args = ["-DBUILD_TESTING=OFF"]    # from the project — REPLACED, not appended
```

Arrays replace rather than append on purpose. If they merged element-wise, a
project could never *remove* an argument your global config had added, only add
more. "The nearer layer wins outright" is a rule you can predict without
reading this page again.

---

## The TOML kap understands

kap has its own small TOML parser (no third-party libraries — that is the whole
project). It handles what configuration needs:

```toml
[section]                   # tables, including dotted: [plugins.cmake-cpp]
key = "string"              # with \" \\ \n \t escapes
count = 42                  # 64-bit integers
flag = true                 # booleans
list = ["a", "b"]           # arrays of scalars
# comments
```

Deliberately **not** supported, and *rejected with a line and column* rather
than misparsed: floats, dates and times, quoted or dotted keys (`"a.b" = 1`),
inline tables (`{ a = 1 }`), arrays of tables (`[[x]]`), `'single-quoted'`
strings, `"""multi-line"""` strings, `\uXXXX` escapes, and hex/octal/binary
integers.

Rejecting rather than ignoring is the point: a construct kap does not
understand today cannot silently change meaning if it is supported tomorrow.

```console
$ kap config get anything
kap: error: kap.toml:4:13: floating-point values are not supported
```

---

## Files kap writes

| Path | What it is | Safe to delete? |
|---|---|---|
| `./.kap/cache.json` | Which plugin claimed this directory | Yes — recomputed |
| `./.kap/.gitignore` | Keeps the above out of your commits | Yes, but it will come back |
| `~/.cache/kap/ast/*.kapc` | Compiled plugins, so they are not re-parsed every run | Yes |
| `~/.cache/kap/plugins-src/` | Scratch space for `git clone` during install | Yes |
| `~/.local/share/kap/plugins/` | Installed plugins | Only via `kap plugin remove` |
| `~/.local/share/kap/installed-plugins.toml` | Where each installed plugin came from | No — it is the record |

`.kap/` ignores itself, so kap's cache never lands in a commit. The one
exception is `.kap/plugins/`, which is where a project-local plugin lives and
*should* be committed.
