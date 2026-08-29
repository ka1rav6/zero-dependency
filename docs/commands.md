# kap — complete command reference

Every command, every flag, every exit code. If you want the guided version
instead, read [usage.md](usage.md); for configuration, [configuration.md](configuration.md).

Everything here is also available from the terminal:

```sh
kap help                  # the index
kap help <command>        # one page
kap <command> --help      # the same page
kap help plugin install   # subcommands too
```

---

## Contents

**Shape of a command line** ·
[Synopsis](#synopsis) ·
[Global flags](#global-flags) ·
[`--` and tool arguments](#--and-tool-arguments) ·
[Exit codes](#exit-codes)

**Project commands** —
[build](#kap-build) ·
[check](#kap-check) ·
[test](#kap-test) ·
[lint](#kap-lint) ·
[fmt](#kap-fmt) ·
[run](#kap-run) ·
[dev](#kap-dev) ·
[clean](#kap-clean) ·
[install](#kap-install) ·
[ci](#kap-ci) ·
[doctor](#kap-doctor) ·
[ports](#kap-ports)

**kap's own commands** —
[detect](#kap-detect) ·
[config get](#kap-config-get) ·
[config set](#kap-config-set) ·
[config edit](#kap-config-edit) ·
[completions](#kap-completions) ·
[help](#kap-help)

**Plugin management** —
[plugin list](#kap-plugin-list) ·
[plugin search](#kap-plugin-search) ·
[plugin install](#kap-plugin-install) ·
[plugin remove](#kap-plugin-remove) ·
[plugin update](#kap-plugin-update) ·
[plugin enable / disable](#kap-plugin-enable--kap-plugin-disable) ·
[plugin pin](#kap-plugin-pin) ·
[plugin new](#kap-plugin-new) ·
[plugin doctor](#kap-plugin-doctor) ·
[plugin test](#kap-plugin-test)

**Reference** —
[Environment variables](#environment-variables) ·
[Files kap reads and writes](#files-kap-reads-and-writes) ·
[Where plugins are found](#where-plugins-are-found)

---

## Synopsis

```
kap [global flags] <command> [command options] [-- <tool arguments>]
kap --version | --help
```

There are three kinds of command:

**Project commands** — `build`, `check`, `ci`, `clean`, `dev`, `doctor`, `fmt`,
`install`, `lint`, `ports`, `run`, `test`. What each one *does* is decided
entirely by the plugin that claims your directory. kap contains no knowledge of
CMake, Cargo, npm, or anything else.

**kap's own commands** — `detect`, `config`, `plugin`, `completions`, `help`.
These are implemented in the binary because they are about kap itself.

**`kap install <name>`** is both: with no arguments it is a project command,
with an argument it manages plugins. See [its section](#kap-install).

---

## Global flags

Recognised before or after the command word, and before `--`.

| Flag | Effect |
|---|---|
| `-n`, `--dry-run` | Render what would run and execute nothing. Also works for `config set`, `plugin install`, `plugin new`, and every other command with a side effect. |
| `--verbose` | Narrate every stage. See [what --verbose shows](#what---verbose-shows). |
| `--root <path>` | Operate on that directory instead of the current one. `--root=<path>` also works. |
| `--set key=value` | Override one of the matched plugin's configuration keys, for this invocation only. Repeatable. |
| `-h`, `--help` | Before a command: kap's usage. After a command: that command's page. |
| `-V`, `--version` | The version, then exit 0. |

An **unknown** flag *before* the command word is an error — there is nothing it
could belong to. After the command word it is forwarded to the command, which
rejects what it does not recognise. That split is what lets `kap detect
--refresh` and `kap plugin install --link` exist without the global parser
having to know every subcommand's options.

### `--set` type conversion

The command line has no types, so kap converts the text using the plugin's
schema:

| Schema type | You write | The plugin receives |
|---|---|---|
| `str` | `--set build_dir=out` | `"out"` |
| `int` | `--set jobs=8` | `8` |
| `bool` | `--set release=true` | `true` |
| `enum` | `--set generator=ninja` | `ninja`, checked against the members |
| `list<str>` | `--set cmake_args=-DA=1,-DB=2` | `["-DA=1", "-DB=2"]` |
| `list<int>` | `--set levels=1,2,3` | `[1, 2, 3]` |

Only the **first** `=` splits key from value, so `--set cmake_args=-DA=1` gives
you `-DA=1`. List values split on commas, so a value containing a comma has to
come from a configuration file instead.

An unknown key, or a value that cannot be the declared type, is an error naming
the key — and for an enum, naming the members that would have worked.

### What `--verbose` shows

```console
$ kap build -n --verbose
kap: root: /home/you/code/demo
kap: config: project /home/you/code/demo/kap.toml
kap: config: hook pre_build = echo starting
kap: plugin: cargo-rust  [bundled] /usr/share/kap/plugins/cargo-rust
kap: plugin: cmake-cpp   [bundled] /usr/share/kap/plugins/cmake-cpp
kap: matched: cmake-cpp  priority=30 score=1 (primary)  markers: CMakeLists.txt
kap: matched: doctor  priority=1 score=1 (composable)  markers: .
kap: detect: scanned, root /home/you/code/demo
kap: using: cmake-cpp /usr/share/kap/plugins/cmake-cpp/plugin.kpl
kap: config: cmake-cpp.build_dir = "out"
kap: config: cmake-cpp.generator = "auto"
kap: spec: 3 step(s)
```

In order: the root, every configuration file read, the detect settings and
hooks in force, every plugin considered and where it came from, every match
with its priority and markers, whether the answer was cached, the plugin
chosen, the **fully resolved configuration record**, and the shape of the
resulting command list.

That configuration block is the most useful part: it is the merge of four
layers, and no single file shows what actually won.

All of it goes to **stderr**, so `kap config get x --verbose` is still safe to
pipe.

---

## `--` and tool arguments

Everything after `--` is passed to the underlying tool untouched:

```sh
kap build -- --target install      # cmake --build build --target install
kap test  -- --nocapture           # cargo test --nocapture
kap run   -- --port 8080
kap test  -- --help                # asks the TEST RUNNER for help, not kap
```

A bare tool argument is refused rather than guessed at:

```console
$ kap build --target install
kap: error: unexpected argument '--target'
      note: arguments for the underlying tool go after '--':
      note:     kap build -- --target install
```

That looks fussy for one line. It is the reason `kap build --release` can never
come to mean two different things on the day kap grows a `--release` of its own.

Which step receives them is the plugin's decision, and a sensible plugin picks
the one you meant — `cmake-cpp` appends to the *build* step, not the configure
step.

---

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Success. |
| `1` | The command failed: nothing matched, a configuration key was wrong, a plugin was broken, a search found nothing. |
| `2` | You typed something kap did not understand: an unknown command, an unknown option, the wrong number of arguments. |
| *n* | Whatever the underlying tool exited with. kap never swallows it. |
| `127` | A tool could not be started at all — with a message saying which, not a bare status. |
| `128+`*n* | The tool was killed by signal *n*, the convention every POSIX shell uses. |

```console
$ kap build; echo $?
CMake Error at CMakeLists.txt:1 ...
1
```

That `1` is cmake's. kap has no exit code of its own for "the build failed",
because inventing one would throw away the information you wanted.

---
---

# Project commands

Every command in this section shares the same global flags, the same `--`
handling, and the same exit codes, so those are not repeated below.

All of them dispatch to whichever plugin claims the directory. If the matched
plugin does not define the command, kap says so and lists the ones it does:

```console
$ kap dev
kap: error: no matched plugin defines a 'dev' command
      note: 'cmake-cpp' claims /home/you/code/demo
      note: available: build check clean doctor fmt install lint ports run test
```

(`doctor` and `ports` are in that list because the two composable system
plugins contribute their commands alongside the plugin that owns the directory.)

---

## `kap build`

Compile the project.

```
kap build [-n] [--verbose] [--root <path>] [--set k=v]... [-- <tool arguments>]
```

| Plugin | Runs |
|---|---|
| `cmake-cpp` | `mkdir -p <build_dir>`, `cmake -S . -B <build_dir> [-G ...] -DCMAKE_BUILD_TYPE=...`, `cmake --build <build_dir>` |
| `cargo-rust` | `cargo build [--release] [--features ...]` |
| `go` | `go build [-tags ...] [-o ...] <packages>` |
| `node` | `<pm> run build`, where `<pm>` comes from the lockfile |
| `python-uv` | `uv build` |
| `make-generic` | `make [-f ...] [-j ...] <build_target>` |

```sh
kap build                          # build it
kap build -n                       # show the commands, run nothing
kap build --set build_dir=out      # somewhere else, once
kap build -- --target install      # pass an argument to the build tool
kap build --root ../other          # build a different directory
```

**Behaviour on failure.** Steps run in order and stop at the first failure — a
configure step that failed has not earned its build, and running on would bury
the error that matters under a second one.

---

## `kap check`

Check the project without building it: the cheapest thing that catches a
mistake.

```
kap check [options] [-- <tool arguments>]
```

| Plugin | Runs |
|---|---|
| `cargo-rust` | `cargo check` |
| `go` | `go vet <packages>` |
| `cmake-cpp` | the configure step only — catches a broken `CMakeLists.txt`, an unfindable dependency, a missing toolchain |
| `python-uv` | the configured type checker, or `ruff check` |
| `node` | `<pm> run typecheck` |

---

## `kap test`

Run the tests.

```
kap test [options] [-- <tool arguments>]
```

```sh
kap test
kap test -- --nocapture            # cargo test --nocapture
kap test -- -R integration         # ctest -R integration
kap test -- --watch                # whatever your runner supports
```

The test runner's own exit code is what kap exits with, so `kap test` drops
into a CI pipeline unchanged.

---

## `kap lint`

Run the linter.

```
kap lint [options] [-- <tool arguments>]
```

| Plugin | Runs |
|---|---|
| `cargo-rust` | `cargo clippy -- -D warnings` |
| `go` | `golangci-lint run` when installed, else `go vet` |
| `python-uv` | `uv run ruff check <paths>` (or flake8) |
| `node` | `<pm> run lint` |
| `cmake-cpp` | `clang-tidy -p <build_dir>` over `format_glob` |

`cmake-cpp` needs `format_glob` set before it does anything. That is
deliberate: a CMake tree routinely contains vendored sources and generated
output, and reporting on those would be worse than doing nothing.

---

## `kap fmt`

Format the source in place.

```
kap fmt [options] [--set check=true] [-- <tool arguments>]
```

To **check** formatting instead of changing it — what a CI job wants:

```sh
kap fmt --set check=true
```

`kap ci` sets that for you when the plugin's schema declares a boolean `check`
field. That is the difference between a pipeline that verifies formatting and
one that silently rewrites your checkout.

> **`go` caveat.** `gofmt` exits 0 even when `-l` printed filenames, so a CI job
> that only inspects the exit code will not notice. That is gofmt's behaviour,
> not kap's. The usual answer: `test -z "$(kap fmt --set check=true)"`.

---

## `kap run`

Run what the project produces.

```
kap run [options] [-- <program arguments>]
```

```sh
kap run
kap run -- --port 8080             # arguments after -- reach your program
kap run --set run_target=demo      # cmake-cpp needs to be told which binary
```

Some ecosystems cannot tell kap which binary you meant. `cmake-cpp` requires
`run_target` rather than guessing, because a wrong guess is worse than a clear
question. Set it permanently in `kap.toml`:

```toml
[plugins.cmake-cpp]
run_target = "demo"
```

---

## `kap dev`

Run the development loop.

```
kap dev [-o|--open] [options]
```

| Option | Effect |
|---|---|
| `-o`, `--open` | Open the first URL any step prints, once. |

In a single-package project this runs one command and gets out of the way. In a
workspace it runs **all of them at once**, prefixing every line with the package
that produced it:

```console
$ kap dev
packages/api | listening on :3001
packages/web | vite v5.4.2  ready in 243 ms
packages/web |   ➜  Local:   http://localhost:5173/
packages/api | GET /health 200
```

Ctrl-C stops all of them; kap forwards the signal to every child. A failing
step does not kill the others — `kap dev` killing three healthy dev servers
because a fourth exited would be worse than useless — and kap reports the first
failure once everything has finished.

**What `-o` costs.** To find a URL kap has to *read* the output, which means
piping it rather than handing over your terminal. A tool that checks whether it
is talking to a terminal will drop its colours. Use `-o` when you want the
browser; leave it off when you want the prettier output.

```console
$ kap dev -o
kap: opened http://localhost:5173/
```

kap opens at most one URL per run, using `xdg-open` or `open`. On a machine
with neither it says so rather than silently doing nothing.

---

## `kap clean`

Remove build output, and report how much space that recovered.

```
kap clean [options]
```

```console
$ kap clean
kap: freed 148.4 MiB
```

The measurement looks at the paths named in the steps' arguments before and
after. A command with no path-shaped argument — `cargo clean`, `go clean` —
falls back to measuring the whole project root. Symlinks are counted as links
and never followed, so a link into `/usr` can never be reported as space kap
freed.

Plugins deliberately leave the expensive-to-restore directories alone.
`node_modules` and `.venv` are technically build output, and deleting them turns
a five-second clean into a five-minute reinstall. Add them if you want them:

```toml
[plugins.node]
clean_paths = ["dist", "build", "node_modules"]
```

---

## `kap install`

**This command means two things**, and which one you get depends on whether you
named something.

```
kap install                        # install the PROJECT
kap install [options] <name|url|path>...   # install a PLUGIN
```

With **no arguments** it is a project command: it installs the project in this
directory, the way `cargo install --path .`, `cmake --install`, `go install`,
or `npm install` would.

With **an argument** it installs a plugin, because that is what everyone means
when they type it. It is exactly `kap plugin install`; see
[that section](#kap-plugin-install) for every option.

```console
$ kap install cmake-cpp
kap: note: 'kap install cmake-cpp' means 'kap plugin install cmake-cpp'
      note: bare 'kap install' installs the project itself
```

The project command takes no positional arguments at all (tool arguments go
after `--`), so `kap install <word>` had no other possible meaning. It used to
be an error whose advice made things worse — it suggested `kap install --
cmake-cpp`, a different wrong thing.

```sh
kap install                        # install this project
kap install -- --prefix=/opt       # ...passing --prefix to the tool
kap install cmake-cpp              # install the cmake-cpp plugin
kap install ./my-plugin            # install a local plugin
```

---

## `kap ci`

The checks a pipeline should run.

```
kap ci [options]
```

If the matched plugin defines its own `ci` command, that is what runs — most
first-party plugins do, because the right order and flags are
ecosystem-specific.

Otherwise kap composes it: whichever of `fmt`, `lint`, and `test` the plugin
defines, **in that order**, stopping at the first failure. For the `fmt` phase
only, if the plugin's schema declares a boolean `check` field, kap sets it, so
CI verifies formatting rather than rewriting the checkout.

`kap ci` takes no tool arguments: they would mean something different to each
phase.

```yaml
# .github/workflows/ci.yml
- run: kap ci
```

---

## `kap doctor`

Check that the tools this project needs are installed.

```
kap doctor [options] [--set strict=true]
```

```console
$ kap doctor
kap doctor
  plugin   cmake-cpp
  plugin   doctor
  plugin   ports
  ok       cmake
  ok       ss
  ok       ninja  (optional)
  --       ccache  (optional, not installed)
healthy
```

**Exit status is 0 when healthy and 1 when a required tool is missing**, so
this works as a CI gate and not only as something to read. A missing *optional*
tool is reported but does not fail: ccache not being installed is a slower
build, not a broken one. `--set strict=true` makes it fail anyway.

`doctor` is itself a plugin, written in KPL. kap collects what every matched
plugin declares in its `requires` block and hands the list over; the plugin
decides what to print, what counts as healthy, and what to exit with.

An `any_of` group — "cmake needs one of these" — is satisfied by any one member
being installed, and reported as a group when none is:

```
  MISSING  ss,lsof,netstat  (need any one of these)
```

---

## `kap ports`

Show which local ports are listening, and what is holding them.

```
kap ports [options] [-- <tool arguments>]
```

| `--set` | Effect |
|---|---|
| `tool=ss\|lsof\|netstat` | Choose the tool instead of detecting it |
| `udp=true` | Include UDP as well as TCP |
| `listening_only=false` | Include established connections |
| `show_process=false` | Skip the owning-process column |

`kap ports -n` prints the exact command first, which is the quickest way to see
what kap chose.

`show_process` usually needs root for sockets you do not own. Without the
privilege the tools omit the column rather than failing, so it is on by default.

---
---

# kap's own commands

## `kap detect`

Show which plugin claims this directory, and why.

```
kap detect [-r|--refresh] [--root <path>]
```

```console
$ kap detect
cargo-rust  priority=40 score=1
  markers: Cargo.toml
  source: bundled (/usr/share/kap/plugins/cargo-rust)
doctor  priority=1 score=1 (composable)
  markers: .
  source: bundled (/usr/share/kap/plugins/doctor)
  root:   /home/you/code/my-crate
  cache:  hit
```

| Field | Meaning |
|---|---|
| `priority` | The manifest number that decides who wins. Higher wins. |
| `score` | How many of that plugin's detect rules fired. Diagnostic detail, **not** a tiebreaker. |
| `markers` | The files that made it match. |
| `(composable)` | Rides alongside the winner instead of competing with it. |
| `source` | Which tier the plugin was found in, and its path. |
| `root` | The directory detection settled on — may differ from yours if `max_walk_up` is set. |
| `cache` | `hit` or `miss (rescanned)`. |

**Exit codes.** 0 when a plugin claims the directory; 1 when none does; 1 with
an explanation when two match at the same priority.

A composable match is **not** ownership. `doctor` and `ports` claim every
directory, so a resolution with no primary still means "nothing owns this":

```console
$ kap detect
kap: error: no plugin claims /tmp/scratch
      note: these composable sidecars matched, and their commands are available: doctor ports
      note: considered: cargo-rust cmake-cpp doctor go make-generic node ports python-uv
```

A tie is refused rather than guessed at, with the fix spelled out:

```console
kap: error: cannot tell which plugin owns /home/you/code/hybrid
      note: these plugins matched at the same priority (30): alpha, beta
      note: pin one in kap.toml:
      note:     [detect]
      note:     ecosystem = "alpha"
```

**`--refresh`** ignores `.kap/cache.json` and rescans. You need it in one case
the cache cannot see: a marker created deep inside a subdirectory that no
existing rule already watches. Deleting `.kap/cache.json` is always safe.

**Searching upward.** By default kap looks only at the directory you are in. To
let it walk up the way git finds `.git`:

```toml
[detect]
max_walk_up = 3
```

Off by default because it can surprise you: run `kap build` in an unrelated
directory inside a monorepo and you would build the monorepo.

---

## `kap config get`

Read one configuration value.

```
kap config get [--global|--project] <key>
```

Reads the **merged** view by default — schema defaults, then
`~/.config/kap/config.toml`, then `./kap.toml` — because "what will kap
actually do" is the question you have.

| Flag | Reads |
|---|---|
| *(none)* | The merged result |
| `--global` | Only `~/.config/kap/config.toml` |
| `--project` | Only `./kap.toml` |

```console
$ kap config get plugins.cmake-cpp.generator
ninja
$ kap config get --global plugins.cmake-cpp.generator
make
```

Values go to **stdout** and diagnostics to **stderr**, so this is safe to pipe.
Arrays print space-separated on one line.

Exit 1 when the key is absent or no configuration file exists.

---

## `kap config set`

Write one configuration value.

```
kap config set [--global|--project] <key> <value>
```

Writes `./kap.toml` by default, `~/.config/kap/config.toml` with `--global`.
Creates the file and any intermediate tables.

**Type inference**: `true`/`false` become booleans, a plain number becomes an
integer, everything else stays a string. So `kap config set detect.max_walk_up
3` stores the number, which is what you meant.

```console
$ kap config set plugins.cmake-cpp.build_dir out
set plugins.cmake-cpp.build_dir in /home/you/code/demo/kap.toml
$ kap -n config set plugins.cmake-cpp.build_dir out
would set plugins.cmake-cpp.build_dir = out in /home/you/code/demo/kap.toml
```

> **Comments and layout are not preserved.** `set` rewrites the whole file
> through kap's TOML writer, which round-trips *values* exactly and nothing
> else. If your `kap.toml` is carefully commented, use `kap config edit`.

kap refuses to rewrite a file it could not parse, rather than replacing it with
its own (empty) understanding of it.

---

## `kap config edit`

Open a configuration file in your editor.

```
kap config edit [--global|--project]
```

Uses `$VISUAL`, then `$EDITOR`. Creates the file empty first if it does not
exist, so there is somewhere to type.

kap will not guess at `vi` or `nano`: launching an editor nobody asked for is a
genuinely hostile surprise in a terminal, and the fix is one line you should
know about anyway.

```console
$ kap config edit
$ EDITOR=code kap config edit --global
```

`$EDITOR` may carry arguments (`code --wait`), as it does for every other tool
that honours the variable.

---

## `kap completions`

Print a shell completion script.

```
kap completions <bash|zsh|fish>
```

```sh
kap completions bash > ~/.local/share/bash-completion/completions/kap
kap completions zsh  > "${fpath[1]}/_kap"
kap completions fish > ~/.config/fish/completions/kap.fish
```

For zsh, restart the shell (or run `compinit`) afterwards.

The scripts are generated from the same command lists the CLI dispatches on, so
they cannot fall out of date with the binary that printed them. Completion is
static: the scripts never shell out to kap on Tab, which would make the shell
feel broken the first time kap was slow.

---

## `kap help`

```
kap help [<command>]
kap <command> --help
```

With no argument, lists every command with a one-line summary. With one, prints
that command's full page. Subcommands work: `kap help plugin install`.

---
---

# Plugin management

```
kap plugin <subcommand> [options]
```

## `kap plugin list`

```
kap plugin list [--verbose]
```

```console
$ kap plugin list
  cargo-rust    1.0.0  [bundled/local]
  cmake-cpp     1.2.0  [user/registry]
! node          1.0.0  [user/script]      disabled
  my-thing      0.1.0  [user/link]        pinned=0.1.0
```

The bracket is **source/origin**:

| Source — where the file was found | |
|---|---|
| `project` | `./.kap/plugins/` |
| `path` | a `$KAP_PLUGIN_PATH` entry |
| `user` | `~/.local/share/kap/plugins/` |
| `bundled` | `<prefix>/share/kap/plugins/` |
| `repo` | `./plugins/` |
| `embedded` | compiled into the binary |

| Origin — how it got there | |
|---|---|
| `registry` | resolved through the registry index |
| `git` | a git URL |
| `script` | an installer script |
| `embedded` | out of the binary |
| `link` | `--link`, a symlink to a working copy |
| `local` | copied from a directory, or never formally installed |

A leading `!` marks a disabled plugin. `--verbose` adds each plugin's full path
and a legend.

---

## `kap plugin search`

```
kap plugin search <query>
```

Matches the query against every registry entry's name, description, and tags,
so searching for `rust` finds `cargo-rust` whichever of the three the word is
in. Matching is a plain lower-cased substring test, not fuzzy: a search that
returns things you did not ask for is worse than one that returns nothing.

Exit 1 when nothing matches.

The index is found in the first of these that exists:

```
$KAP_REGISTRY
~/.local/share/kap/registry/index.toml
<project>/registry/index.toml
<prefix>/share/kap/registry/index.toml
the copy compiled into the binary          ← always present
```

The last line means search always works, even on a binary with nothing beside
it.

---

## `kap plugin install`

```
kap plugin install [options] <name|url|path>...
kap plugin install [options] --bundle <bundle>
```

| Option | Effect |
|---|---|
| `-y`, `--yes` | Do not ask for confirmation. |
| `--link` | Symlink a local directory instead of copying it. |
| `--project` | Install into `./.kap/plugins` instead of your home directory. |
| `--force` | Reinstall over an existing copy. |
| `--bundle <b>` | Install a named set: `core`, `system`, `cpp`, `web`. |

### The five sources, resolved in this order

**1. A plugin compiled into this binary.** On a `-DKAP_EMBED_PLUGINS=ON` build,
first-party plugins install instantly and offline. Checked first because the
embedded copy *is* what this binary ships — fetching possibly different text
over the network would be the surprising choice.

**2. A name in the registry index.** kap uses the entry's `install_script` when
it has one, and otherwise does a shallow git clone at the pinned `ref`.

**3. An installer-script URL**, for a plugin hosted anywhere:

```sh
kap plugin install https://example.com/kap-zig/install.sh
```

kap downloads it, shows the URL, size, and SHA-256, tells you where it saved it
so you can read it, and asks before running it:

```console
run an installer script from the network
  url:      https://example.com/kap-zig/install.sh
  size:     461 bytes
  sha256:   93f8c0f7f8b3a235deeed17c4e5e7c9564ba15bbce59488a2295e62456bc3a86
  saved at: /home/you/.cache/kap/plugins-src/install.staging.install.sh

  This script runs as you, with your permissions. Read it before saying
  yes if you did not write it. kap will still validate whatever it
  produces before installing anything.

Proceed? [y/N]
```

The script writes files into a staging directory and decides nothing. Whatever
it produces goes through the same validation as every other install.

**HTTPS only.** kap executes what it downloads from an installer URL, and plain
HTTP can be replaced by anyone on the path between you and the server. Loopback
is exempt, because there is no such path.

Writing one is documented in [plugins.md](plugins.md#publishing-to-a-registry).

**4. A git URL** — `https://`, `git@`, `ssh://`, `git://`, `file://`, or
anything ending in `.git`. Cloned shallow.

**5. A local directory** containing a `plugin.kpl`.

### What happens before anything is written

1. Fetch into a staging directory.
2. `plugin.kpl` parses.
3. The manifest has `name`, `version`, and `api_version`.
4. `api_version` is one this kap supports.
5. The detect rules are well-formed.
6. Every command type-checks.
7. The SHA-256 matches the registry index, when the entry carries one. **A
   mismatch fails the install**, it does not warn.
8. A summary is printed and confirmed.
9. The plugin is copied beside its destination and renamed into place, so a
   failure partway through cannot leave a half-installed plugin.
10. The lockfile is updated and the detection cache invalidated.

Without `--yes`, a non-interactive stdin is treated as **no**, never yes.
Installing third-party code because nobody was there to object is the exact
failure the prompt exists to prevent.

### Examples

```sh
kap plugin install cmake-cpp
kap plugin install --bundle core           # all six ecosystems
kap plugin install --bundle system         # doctor and ports
kap plugin install --link ./my-plugin      # develop it: edits are live
kap plugin install --project ./my-plugin   # commit it with the repository
kap plugin install https://example.com/kap-zig/install.sh
kap plugin install https://github.com/someone/kap-zig
kap plugin install --force --yes cmake-cpp
```

`--project` is the one to know for a team: the plugin lands in
`./.kap/plugins/`, which is committed, so everyone who clones the repository
gets it without installing anything.

---

## `kap plugin remove`

```
kap plugin remove <name>
```

Deletes the plugin's directory and its lockfile row. Exit 1 if it is not
installed.

A `--link` install is a symlink: removing it removes the **link** and leaves
your working copy exactly where it was.

`kap plugin uninstall` is accepted as a synonym.

---

## `kap plugin update`

```
kap plugin update [name]
```

Re-runs the install pipeline for one plugin, or for every installed plugin when
given no name, keeping each one's original source.

Two kinds are skipped, with a reason:

- **pinned** — unpin it with `kap plugin pin <name> --clear` first.
- **linked** — a symlink to your working copy is always current; there is
  nothing to fetch.

Naming a plugin explicitly makes those an **error**; updating everything reports
them and carries on, because you did not ask about that one in particular.

---

## `kap plugin enable` / `kap plugin disable`

```
kap plugin enable <name>
kap plugin disable <name>
```

A disabled plugin is skipped by detection, so it can never claim a directory —
but its files stay where they are, `kap plugin doctor` still checks it, and
enabling brings it straight back.

The state lives in the lockfile rather than in the filesystem, which is what
makes it reversible and keeps a switched-off plugin inspectable.

Useful when two plugins fight over the same project and you want one out of the
way without deciding to delete it.

A plugin that was never formally installed — a bundled or in-repo one — can
still be toggled; kap creates a lockfile row for it.

---

## `kap plugin pin`

```
kap plugin pin <name> <version>
kap plugin pin <name> --clear
```

A pinned plugin is skipped by `kap plugin update`, which reports the pin rather
than quietly ignoring it. Use it when a newer version broke something and you
have not worked out why yet.

Exit 1 if the plugin is not installed: only an installed plugin has a version to
pin.

---

## `kap plugin new`

```
kap plugin new <name> [--template build-system]
```

Writes a complete, working plugin:

```
<name>/plugin.kpl                                the whole plugin
<name>/README.md                                 what it does, how to configure it
<name>/tests/fixtures/example/                   a fake project
<name>/tests/expected/example.build.steps.json   the commands it should produce
```

It is valid immediately — `kap plugin doctor <name>` passes and `kap plugin test
<name>` has a passing case — so the first thing you do is change something, not
debug the template.

`--dry-run` reports what would be created and writes nothing. Exit 1 if the
directory exists or the template is unknown.

See [plugins.md](plugins.md) for the guide.

---

## `kap plugin doctor`

```
kap plugin doctor [<name|path>...]
```

With no argument, checks every plugin kap can see. With names or paths, checks
those.

A **path** works, so you can check a plugin before installing it — which is what
you want right after `kap plugin new`. Needing to install something before you
could syntax-check it would be backwards.

Each plugin is checked for everything that would stop it running: the file
parses, the manifest has what an install requires, its `api_version` is
supported, its detect rules are well-formed, and every command type-checks. A
plugin whose manifest is fine but whose `build` command reads an undeclared
config key is broken, and reporting it as healthy would be a lie.

Errors carry the file, line, and column.

Exit 1 if any plugin fails.

```console
$ kap plugin doctor ./my-plugin
[FAIL] my-plugin
kap: error: ./my-plugin/plugin.kpl:
  ./my-plugin/plugin.kpl:14:22: unknown config key 'nope'; it is not declared in the schema block
```

---

## `kap plugin test`

```
kap plugin test [<name|path>...]
```

Evaluates a plugin's command blocks against fixture project trees and compares
the resulting command lists with committed golden files.

**Nothing is executed.** That is the point: kap's own CI tests all eight bundled
plugins with no cmake, cargo, npm, go, or uv installed anywhere. A case declares
what `project.tool()` and `project.env()` should report, so a test cannot pass
on your laptop and fail in CI.

```
tests/fixtures/<fixture>/                        a fake project tree
tests/expected/<fixture>.<command>.steps.json    one case
```

A failure prints both the expected and the actual command list **in full**,
because "it differs" is not actionable.

Exit 1 if any case fails. A plugin with no cases is reported as `[SKIP]`, not as
a pass.

The case-file format is documented in
[plugins.md](plugins.md#testing-without-running-anything).

---
---

# Reference

## Environment variables

| Variable | Effect |
|---|---|
| `KAP_PLUGIN_PATH` | Extra plugin directories, colon-separated. Searched above your installed plugins. |
| `KAP_REGISTRY` | Path to a registry `index.toml`, overriding the usual search. |
| `KAP_BUNDLED_PLUGIN_DIR` | Override where the bundled plugins are looked for. |
| `KAP_NO_EMBEDDED_PLUGINS` | Any value: ignore plugins compiled into the binary. Answers "why is kap using a plugin I never installed?" |
| `NO_COLOR` | Any value turns off colour. |
| `EDITOR`, `VISUAL` | Used by `kap config edit`. `VISUAL` wins. |
| `XDG_CONFIG_HOME` | Where `kap/config.toml` lives. Default `~/.config`. |
| `XDG_DATA_HOME` | Installed plugins and the lockfile. Default `~/.local/share`. |
| `XDG_CACHE_HOME` | Compiled plugins and scratch space. Default `~/.cache`. |
| `HOME` | The fallback for all three above. |
| `PATH` | Scanned by `project.tool()`, and to find `git`, `curl`, `wget`, and the tools plugins run. |

Pointing all three XDG variables at a scratch directory gives you a completely
isolated kap, which is how kap's own tests stay hermetic:

```sh
export XDG_CONFIG_HOME=/tmp/k/config XDG_DATA_HOME=/tmp/k/data XDG_CACHE_HOME=/tmp/k/cache
```

---

## Files kap reads and writes

| Path | What | Safe to delete? |
|---|---|---|
| `./kap.toml` | Project configuration. Committed. | It is yours |
| `~/.config/kap/config.toml` | Your configuration. | It is yours |
| `./.kap/cache.json` | Which plugin claimed this directory. | Yes — recomputed |
| `./.kap/.gitignore` | Keeps the above out of commits. | Yes, it comes back |
| `./.kap/plugins/` | Project-local plugins. **Commit these.** | No |
| `~/.local/share/kap/plugins/` | Installed plugins. | Use `kap plugin remove` |
| `~/.local/share/kap/installed-plugins.toml` | Where each installed plugin came from, plus enable/pin state. | No — it is the record |
| `~/.local/share/kap/registry/` | A fetched registry index. | Yes |
| `~/.cache/kap/ast/*.kapc` | Compiled plugins, so they are not re-parsed each run. | Yes |
| `~/.cache/kap/plugins-src/` | Scratch space for clones and downloaded installers. | Yes |
| `~/.cache/kap/embedded/` | Plugins written out of the binary. | Yes — rewritten |
| `<prefix>/share/kap/plugins/` | Plugins installed alongside the binary. | Part of the install |
| `<prefix>/share/kap/registry/index.toml` | The registry index. | Part of the install |

`.kap/` ignores itself, so kap's cache never lands in a commit. The exception is
`.kap/plugins/`, which is where a project-local plugin lives and *should* be
committed.

---

## Where plugins are found

Searched in this order; the first match for a given name wins.

| # | Tier | Path |
|---|---|---|
| 1 | project | `./.kap/plugins/<name>/` |
| 2 | path | each `$KAP_PLUGIN_PATH` entry |
| 3 | user | `~/.local/share/kap/plugins/<name>/` |
| 4 | bundled | `<prefix>/share/kap/plugins/<name>/` |
| 5 | repo | `./plugins/<name>/` |
| 6 | embedded | `~/.cache/kap/embedded/<name>/` |

**project** is committed with the repository, so a team shares a plugin without
anyone installing anything.

**repo** exists so `kap plugin doctor` works in a checkout that develops plugins
in-tree — kap's own repository, for instance. It sits below the installed tiers
so an installed plugin of the same name still wins.

**embedded** is last of all. An embedded plugin is a snapshot taken when the
binary was compiled, which makes it the weakest claim there is: a distributor's
patched copy should win, and so should the plugin a developer has open in their
checkout.

`kap detect` and `kap plugin list --verbose` both show which tier a plugin came
from.
