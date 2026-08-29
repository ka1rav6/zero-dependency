# Using kap

The guided tour. For an exhaustive, command-by-command reference — every flag,
every exit code, every file — see **[commands.md](commands.md)**, or ask the
binary:

```sh
kap help                  # the index
kap <command> --help      # one command's full page
kap help plugin install   # subcommands too
```

For the shape of a `kap.toml`, see [configuration.md](configuration.md); for
writing your own plugin, [plugins.md](plugins.md).

---

## Contents

- [Installing](#installing)
- [The first five minutes](#the-first-five-minutes)
- [Project commands](#project-commands)
- [Global flags](#global-flags)
- [Passing arguments to the underlying tool](#passing-arguments-to-the-underlying-tool)
- [`kap detect` — who owns this directory](#kap-detect--who-owns-this-directory)
- [When a plugin declines](#when-a-plugin-declines)
- [`kap doctor` — do I have what it needs](#kap-doctor--do-i-have-what-it-needs)
- [`kap dev` — the development loop](#kap-dev--the-development-loop)
- [`kap ci` — one command for a pipeline](#kap-ci--one-command-for-a-pipeline)
- [`kap config` — reading and writing settings](#kap-config--reading-and-writing-settings)
- [`kap plugin` — managing plugins](#kap-plugin--managing-plugins)
- [Shell completions](#shell-completions)
- [Exit codes](#exit-codes)
- [Environment variables](#environment-variables)
- [When something goes wrong](#when-something-goes-wrong)

---

## Installing

### The install script

```sh
curl -fsSL https://raw.githubusercontent.com/ka1rav6/zero-dependency/main/scripts/install.sh | sh
```

For a **single self-contained binary** — the plugins compiled in, nothing
needed beside it, no network ever:

```sh
curl -fsSL .../install.sh | KAP_EMBED=1 sh
```

That is the one to use if you will copy `kap` to a server, into a container, or
onto a machine you do not administer. The default installs the plugins as text
files next to the binary, which is what a distribution package wants.

It clones the repository, builds, **runs the test suite**, and installs into
`~/.local` — the binary, the ten bundled plugins, and the registry index. A
build that fails its own tests is not installed.

#### Where the plugins end up

Two places, on purpose:

| Path | Who reads it |
|---|---|
| `<prefix>/share/kap/plugins/<name>/` | only the binary at `<prefix>/bin/kap`, which finds it relative to itself |
| `~/.local/share/kap/plugins/<name>/` | every kap on the machine, wherever its binary lives |

The first is the packaging layout, and it is the one a distribution wants. The
second is what makes the plugins *installed globally*: a second kap — built in
a checkout, dropped in `/usr/local/bin` by a colleague, copied out of a
container — finds them without being told where to look. The user copy also
outranks the packaged one, so a plugin you edit there stays in charge across
reinstalls.

`--embed` skips the user copy: those plugins are inside the binary already, and
a second copy on disk would silently take precedence over it.

`~/.local/share/kap/registry/index.toml` is written the same way, so
`kap plugin search` works from any kap too.

Options, as flags or as environment variables (the second form is what works
through a `curl | sh` pipe):

| Flag | Variable | Default |
|---|---|---|
| `--prefix DIR` | `KAP_PREFIX` | `~/.local` |
| `--ref REF` | `KAP_REF` | `main` |
| `--repo URL` | `KAP_REPO` | the kap project |
| — | `KAP_BUILD_TYPE` | `Release` |

```sh
curl -fsSL .../install.sh | KAP_PREFIX=/usr/local sh
```

You need `git`, `cmake`, and a C++20 compiler. The script checks for all of
them up front and names every one that is missing, rather than failing three
times in a row.

### From source

```sh
git clone https://github.com/ka1rav6/zero-dependency
cd kap
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix ~/.local
```

### Checking it worked

```console
$ kap --version
kap 1.0.0
$ kap plugin list
  cargo-rust    1.0.0  [bundled/local]
  cmake-cpp     1.0.0  [bundled/local]
  doctor        1.0.0  [bundled/local]
  go            1.0.0  [bundled/local]
  make-generic  1.0.0  [bundled/local]
  node          1.0.0  [bundled/local]
  ports         1.0.0  [bundled/local]
  python-uv     1.0.0  [bundled/local]
```

If `kap` is not found, `~/.local/bin` is probably not on your `PATH`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

---

## The first five minutes

```sh
cd ~/code/anything
kap detect      # which plugin claims this, and why
kap doctor      # are the tools it needs installed
kap build -n    # what would `kap build` actually run
kap build       # run it
```

`kap build -n` is worth the habit. kap never runs anything it will not show
you, and `-n` shows it without running it.

---

## Project commands

These twelve are the surface. What each one *does* is entirely up to the plugin
that claims your directory — the binary contains no ecosystem knowledge at all.

| Command | Usually means |
|---|---|
| `kap build` | Compile |
| `kap check` | Typecheck or configure, without producing artifacts |
| `kap test` | Run the tests |
| `kap lint` | Run the linter |
| `kap fmt` | Format the source |
| `kap run` | Run the thing you just built |
| `kap dev` | The development loop — often several servers at once |
| `kap clean` | Remove build output, and report the space recovered |
| `kap install` | Install the project |
| `kap ci` | fmt-check, lint, test — see [below](#kap-ci--one-command-for-a-pipeline) |
| `kap doctor` | Check the tools this project needs |
| `kap ports` | Show what is listening locally |

A plugin need not define all of them. Asking for one it does not have tells you
what it does have:

```console
$ kap dev
kap: error: no matched plugin defines a 'dev' command
      note: 'cmake-cpp' claims /home/you/code/my-project
      note: available: build check clean doctor fmt install lint ports run test
```

`doctor` and `ports` are in that list because the two composable system plugins
contribute their commands alongside whichever plugin owns the directory.

The exact commands each bundled plugin runs are in its README — see the
[table in the docs index](README.md#start-here).

---

## Global flags

Valid before or after the command word.

| Flag | Effect |
|---|---|
| `-n`, `--dry-run` | Print the commands instead of running them |
| `--verbose` | Explain what kap is doing: which config files, which plugin, which steps |
| `--root <path>` | Work on that directory instead of the current one |
| `--set key=value` | Override one plugin config key, for this invocation only. Repeatable |
| `-h`, `--help` | Usage |
| `-V`, `--version` | Version |

```sh
kap build --root ~/code/other-project
kap build --set build_dir=out --set generator=make
kap --verbose test
```

`--set` values are converted to whatever type the plugin's schema declares, so
`--set release=true` produces a boolean and `--set cmake_args=-DA=1,-DB=2`
produces a list. A key the schema does not declare is an error, and the message
names it.

---

## Passing arguments to the underlying tool

Everything after `--` goes to the tool, not to kap:

```sh
kap build -- --target install     # cmake --build build --target install
kap test  -- --nocapture          # cargo test --nocapture
kap run   -- --port 8080
```

A bare tool argument is refused rather than guessed at:

```console
$ kap build --target install
kap: error: unexpected argument '--target'
      note: arguments for the underlying tool go after '--':
      note:     kap build -- --target install
```

That looks fussy for one line, and it is the reason `kap build --release` will
never quietly mean two different things once kap grows a `--release` of its own.

---

## `kap detect` — who owns this directory

```console
$ kap detect
cargo-rust  priority=40 score=1
  markers: Cargo.toml
  source: bundled (/usr/local/share/kap/plugins/cargo-rust)
doctor  priority=1 score=1 (composable)
  markers: .
  source: bundled (/usr/local/share/kap/plugins/doctor)
ports  priority=1 score=1 (composable)
  markers: .
  source: bundled (/usr/local/share/kap/plugins/ports)
  root:   /home/you/code/my-crate
  cache:  hit
```

Reading that:

- **priority** decides who wins when several plugins match. **score** is how
  many of that plugin's rules fired — diagnostic detail, not a tiebreaker.
- **markers** are the files that made it match.
- **composable** plugins ride alongside the winner instead of competing with
  it. `doctor` and `ports` claim every directory, which is how `kap doctor`
  works anywhere.
- **cache** says whether the answer came from `.kap/cache.json`. Use
  `kap detect --refresh` to ignore it.

When nothing claims the directory, kap shows what it tried:

```console
$ kap detect
kap: error: no plugin claims /home/you/code/backend
      note: these composable sidecars matched, and their commands are available: doctor ports
      note: evaluated these plugins but none matched:
      note: go (priority 45)
      note:   ✗ file_exists "go.mod"
      note:   ✗ file_exists "go.work"
      note: cargo-rust (priority 40)
      note:   ✗ file_exists "Cargo.toml"
      note: python-uv (priority 38)
      note:   ✗ file_exists "uv.lock"
      note:   ✗ file_exists "pyproject.toml"
      note: considered: cargo-rust cmake-cpp doctor go java make-generic node ports python-uv zig
```

Each `✗` is a marker a plugin looked for and did not find. Above, the directory
is full of `.py` files but has no packaging metadata, and the `python-uv` lines
say so: give it a `pyproject.toml` — `uv init` will — and kap will claim it.

The same list appears whenever a project command fails for want of an owner, so
you rarely have to run `kap detect` by hand to see it. It is ranked by priority
and capped at three, because detection considers every installed plugin and the
whole list would bury the answer.

If two plugins match at the same priority, kap refuses to guess:

```console
$ kap detect
kap: error: cannot tell which plugin owns /home/you/code/hybrid
      note: these plugins matched at the same priority (30): alpha, beta
      note: pin one in kap.toml:
      note:     [detect]
      note:     ecosystem = "alpha"
```

### Working from a subdirectory

By default kap looks only at the directory you are in. To let it walk upward
the way git finds `.git`:

```toml
# kap.toml
[detect]
max_walk_up = 3
```

---

## When a plugin declines

A plugin can look at your project and conclude that the command you asked for
cannot work. It says so itself, instead of running a tool that is about to fail:

```console
$ kap run
kap: error: package.json declares no "start" script, which is what 'kap run' runs; this project has a "dev" script — try 'kap dev'
```

Nothing was spawned. The exit code is 1, and the message is kap's, not the
tool's — which is what lets it mention the three things only kap knows: that
`kap run` picked the name `start`, that `start_script` in `kap.toml` can point
it elsewhere, and that a sibling kap command already does what you wanted.

You will meet this mostly in JavaScript projects, where there is no standard set
of script names. Every one kap uses is configurable:

```toml
# kap.toml
[plugins.node]
start_script = "serve"
build_script = "compile"
```

`kap <command> -n` shows the same message, so you can check without running
anything. If you would rather have the underlying tool's error, point the
script name at something that exists and let it fail on its own terms.

---

## `kap doctor` — do I have what it needs

```console
$ kap doctor
kap doctor
  plugin   cargo-rust
  plugin   doctor
  plugin   ports
  ok       cargo
  ok       ss
  ok       rustfmt  (optional)
  --       clippy   (optional, not installed)
healthy
```

Exit status is 0 when healthy and 1 when a *required* tool is missing, so it
works as a CI gate and not only as something to read. A missing optional tool
is reported but does not fail — `ccache` not being installed is a slower build,
not a broken one.

`doctor` is itself a plugin. The core collects what every matched plugin
declares in its `requires` block and hands the list over; the plugin decides
what to print and what counts as healthy. See
[kap-plugins/doctor/README.md](../kap-plugins/doctor/README.md).

---

## `kap dev` — the development loop

In a single-package project, `kap dev` runs one command and gets out of the way.

In a workspace it runs **all of them at once**, prefixing every line with the
package that produced it:

```console
$ kap dev
packages/api | listening on :3001
packages/web | vite v5.4.2  ready in 243 ms
packages/web |   ➜  Local:   http://localhost:5173/
packages/api | GET /health 200
```

Ctrl-C stops all of them. `kap dev -n` shows exactly which commands would run.

### `-o` — open the first URL

```console
$ kap dev -o
kap: opened http://localhost:5173/
packages/web | vite v5.4.2  ready in 243 ms
```

One caveat, worth knowing before it surprises you: `-o` has to *read* the
output to find a URL in it, so it pipes every step rather than handing over
your terminal. Tools that check whether they are talking to a terminal will
drop their colours. Use `-o` when you want the browser; leave it off when you
want the prettier output.

---

## `kap ci` — one command for a pipeline

If the plugin defines its own `ci`, that is what runs. Otherwise kap composes
the three phases from [§8 of the design](design.md): whichever of `fmt`,
`lint`, and `test` the plugin defines, in that order, stopping at the first
failure.

For the `fmt` phase only, if the plugin's schema has a boolean `check` field,
kap sets it — the difference between a CI job that *verifies* formatting and
one that silently rewrites your checkout.

```yaml
# .github/workflows/ci.yml
- run: kap ci
```

---

## `kap config` — reading and writing settings

```console
$ kap config get plugins.cmake-cpp.generator     # the effective value
ninja
$ kap config get --global plugins.cmake-cpp.generator   # just your global file
make
$ kap config set plugins.cmake-cpp.build_dir out        # writes ./kap.toml
set plugins.cmake-cpp.build_dir in /home/you/code/my-project/kap.toml
$ kap config set --global detect.max_walk_up 3          # writes ~/.config/kap/config.toml
$ kap config edit                                       # opens $EDITOR
```

`get` reads the **merged** view by default, because "what will kap actually do"
is the question you have. `--global` and `--project` narrow it to one file.

`set` rewrites the file through kap's TOML writer, which round-trips *values*
but not comments or layout. If you care about how your config file looks, use
`kap config edit` and edit it directly.

The full reference is [configuration.md](configuration.md).

---

## `kap plugin` — managing plugins

```console
$ kap plugin list
  cargo-rust  1.0.0  [bundled/local]
! node        1.2.0  [user/registry]  disabled
  my-thing    0.1.0  [user/link]      pinned=0.1.0
```

The bracket is `source/origin`: where the file was found (`project`, `path`,
`user`, `bundled`, `repo`) and where it came from (`registry`, `git`, `link`,
`local`). A leading `!` marks a disabled plugin.

| Command | What it does |
|---|---|
| `kap plugin list` | Everything visible, and its state |
| `kap plugin search <query>` | Search the registry index by name, description, or tag |
| `kap plugin install <name>` | Install from the registry |
| `kap plugin install <git-url>` | Install straight from a repository |
| `kap plugin install <path>` | Install a copy of a local directory |
| `kap plugin install --link <path>` | Symlink a working copy — edits take effect immediately |
| `kap plugin install --bundle <name>` | Install a curated set (`core`, `system`, `cpp`, `web`) |
| `kap plugin remove <name>` | Uninstall (a `--link` install leaves your working copy alone) |
| `kap plugin update [name]` | Re-fetch one plugin, or all of them |
| `kap plugin enable\|disable <name>` | Switch a plugin off without uninstalling it |
| `kap plugin pin <name> <version>` | Stop `update` from moving it (`--clear` to unpin) |
| `kap plugin new <name>` | Scaffold a new plugin |
| `kap plugin doctor [name\|path]` | Parse, validate, and type-check |
| `kap plugin test [name\|path]` | Run a plugin's fixture cases |

Useful flags for `install`: `--yes` (skip the confirmation), `--project`
(install into `./.kap/plugins` instead of your home directory), `--force`
(reinstall over an existing copy).

### What installing shows you

```console
$ kap plugin install cargo-rust
install 'cargo-rust' 1.0.0
  source:      registry https://github.com/ka1rav6/zero-dependency
  ref:         main
  subdirectory: kap-plugins/cargo-rust
  destination: /home/you/.local/share/kap/plugins/cargo-rust
  checksum:    verified

Proceed? [y/N]
```

Before anything is written, kap fetches the payload, checks that it parses,
that its manifest is complete, that its `api_version` is one this kap supports,
that its detect rules are well-formed, and that every command type-checks. Then
it verifies the SHA-256 checksum against the registry index and refuses the
install on a mismatch.

A non-interactive stdin is treated as **no**, never yes. A script that means it
passes `--yes`.

### Where plugins are found

Highest precedence first, so a project-local plugin shadows a user-installed
one of the same name, which shadows one that shipped with the binary:

```
./.kap/plugins/<name>/                  project-local, committed with the repo
$KAP_PLUGIN_PATH                        colon-separated, for development
~/.local/share/kap/plugins/<name>/      kap plugin install
<prefix>/share/kap/plugins/<name>/      shipped with the binary
./kap-plugins/<name>/                   a repository that develops plugins in-tree
./plugins/<name>/                       ...or the plainer name for the same
```

---

## Shell completions

```sh
# bash
kap completions bash > ~/.local/share/bash-completion/completions/kap

# zsh
kap completions zsh > "${fpath[1]}/_kap"

# fish
kap completions fish > ~/.config/fish/completions/kap.fish
```

The scripts are generated from the same command lists the CLI dispatches on, so
they cannot fall out of date with the binary that printed them.

---

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | The command failed: nothing matched, a config key was wrong, a plugin was broken |
| 2 | You typed something kap did not understand |
| *n* | Whatever the underlying tool exited with — kap never swallows it |
| 127 | A tool could not be started at all (with a message saying which) |
| 128+*n* | The tool was killed by signal *n*, as any shell would report it |

```console
$ kap build; echo $?
CMake Error at CMakeLists.txt:1 ...
1
```

That `1` is `cmake`'s own exit status, passed through untouched. A tool that
exits 3 makes kap exit 3; kap has no exit code of its own for "the build
failed", because inventing one would throw away the information you wanted.

---

## Environment variables

| Variable | What it does |
|---|---|
| `KAP_PLUGIN_PATH` | Extra plugin directories, colon-separated. Searched above your installed plugins |
| `KAP_REGISTRY` | Path to a registry `index.toml`, overriding the usual search |
| `KAP_BUNDLED_PLUGIN_DIR` | Override where the bundled plugins are looked for |
| `KAP_NO_EMBEDDED_PLUGINS` | Ignore plugins compiled into the binary. Any value. Useful for answering "why is kap using a plugin I never installed?" |
| `NO_COLOR` | Any value turns off colour |
| `EDITOR` / `VISUAL` | Used by `kap config edit` |
| `XDG_CONFIG_HOME` | Where `kap/config.toml` lives (default `~/.config`) |
| `XDG_DATA_HOME` | Where installed plugins and the lockfile live (default `~/.local/share`) |
| `XDG_CACHE_HOME` | Where the compiled-plugin cache lives (default `~/.cache`) |

Pointing all three XDG variables at a scratch directory gives you a completely
isolated kap, which is how kap's own tests stay hermetic.

---

## When something goes wrong

**"no plugin claims …"** — nothing recognised the directory. The error lists
every marker the closest plugins looked for and did not find, so read those
lines first: usually one of them names a file your project is simply missing
(`pyproject.toml`, `go.mod`, a `Makefile`). Failing that, install a plugin
(`kap plugin install --bundle core`) or write one ([plugins.md](plugins.md)).

**"cannot tell which plugin owns …"** — two plugins matched at the same
priority. Pin one:

```toml
[detect]
ecosystem = "cargo-rust"
```

**"unknown config key 'x'"** — the key is not in that plugin's schema. Its
README lists every key it accepts.

**"cannot run 'cmake': No such file or directory"** — the tool is not
installed. `kap doctor` will tell you everything that is missing at once.

**"package.json declares no 'build' script"** — the plugin checked before
running anything. Either add the script, or point kap at the one you already
have: `kap build --set build_script=compile` for once,
`[plugins.node] build_script = "compile"` in `kap.toml` for good. See
[When a plugin declines](#when-a-plugin-declines).

**"Does not match the generator used previously"** — CMake refusing a `-G` that
contradicts an existing `build/CMakeCache.txt`. kap's `auto` no longer passes
`-G` into a configured directory, so if you still see this you have pinned a
generator explicitly. Either match what the cache holds, or delete the build
directory (`kap clean`) and let it be configured fresh:

```toml
[plugins.cmake-cpp]
generator = "make"      # or "ninja", or drop the key for auto
```

**kap is running the wrong command** — `kap build -n` shows the exact argv
arrays, and `--verbose` adds which config files were read and which plugin was
chosen. Between the two there is nothing hidden.

**A stale answer** — `.kap/cache.json` remembers which plugin owns a directory.
Its key covers the directory listing, every plugin's identity and mtime, and
kap's own version, so installing a plugin, adding a marker file, and upgrading
kap all invalidate it on their own. The one case it cannot see is a marker
created deep in a subdirectory that no existing rule watches. `kap detect
--refresh` forces a rescan, and deleting `.kap/cache.json` is always safe.
