# Writing a kap plugin

A plugin teaches kap about one ecosystem. It is a single text file plus a
README — no C++, no compiler, no rebuild of kap.

This page walks through building one. For the language itself, keyword by
keyword, see [PLUGIN_API.md](PLUGIN_API.md).

---

## Contents

- [Ten minutes to a working plugin](#ten-minutes-to-a-working-plugin)
- [What a plugin is](#what-a-plugin-is)
- [The five blocks](#the-five-blocks)
- [Writing commands](#writing-commands)
- [What a command can see](#what-a-command-can-see)
- [Testing without running anything](#testing-without-running-anything)
- [The development loop](#the-development-loop)
- [Installing and sharing](#installing-and-sharing)
- [Publishing to a registry](#publishing-to-a-registry)
- [What plugins cannot do](#what-plugins-cannot-do)
- [Recipes](#recipes)
- [Mistakes worth avoiding](#mistakes-worth-avoiding)

---

## Ten minutes to a working plugin

```console
$ kap plugin new zig
created /home/you/code/zig
  plugin.kpl                                    the whole plugin
  README.md                                     what it does and how to configure it
  tests/fixtures/example/                       a fake project to evaluate against
  tests/expected/example.build.steps.json       the commands that should produce

next:
  kap plugin doctor zig          parse, validate, and type-check it
  kap plugin test zig            run its fixture cases
  kap plugin install --link zig  install a live-editable symlink
```

Those three commands work immediately — the scaffold is a valid plugin with a
passing test:

```console
$ kap plugin doctor zig
[PASS] zig
$ kap plugin test zig
[PASS] zig example.build
1 cases: 1 passed, 0 failed
```

Now open `zig/plugin.kpl` and make it real. The two edits that matter:

```kpl
detect {
  file_exists "build.zig"       // was: "CHANGE-ME"
}

command build(project, config, extra) {
  step ["zig", "build"] + extra // was: an echo
}
```

Update the golden file to match, and you have a working plugin:

```console
$ cat > zig/tests/expected/example.build.steps.json <<'JSON'
{ "steps": [ { "cmd": ["zig", "build"] } ] }
JSON
$ mv zig/tests/fixtures/example/CHANGE-ME zig/tests/fixtures/example/build.zig
$ kap plugin test zig
[PASS] zig example.build
1 cases: 1 passed, 0 failed

$ kap plugin install --link zig
$ cd ~/code/some-zig-project && kap build -n
$ zig build
```

---

## What a plugin is

```
zig/
├── plugin.kpl      the whole plugin: manifest, detection, config schema, commands
├── README.md       what it claims, what it runs, what you can configure
└── tests/
    ├── fixtures/<name>/                     a fake project tree
    └── expected/<fixture>.<command>.steps.json   the commands it should produce
```

`tests/` is for you and for CI; kap never reads it at run time.

kap's job at run time is: work out which plugin owns the directory, evaluate the
right `command` block, and get back a list of commands. It then runs them. The
plugin itself never runs anything — which is exactly what makes `kap build -n`
able to show you everything first.

---

## The five blocks

### `manifest` — identity and ranking

```kpl
manifest {
  name        = "zig"
  version     = "1.0.0"
  api_version = 1

  // Higher wins when several plugins match the same directory.
  priority    = 35

  // true means "run alongside the winner" rather than compete to be it.
  composable  = false

  // Names of plugins to ignore whenever this one matches.
  supersedes  = []
}
```

For `priority`, the bundled plugins are the yardstick: `make-generic` 10,
`cmake-cpp` 30, `node` 35, `python-uv` 38, `cargo-rust` 40, `go` 45. A generic
fallback belongs well below them; a plugin for a specific framework that sits
on top of one of these belongs above.

If two plugins tie, kap refuses to guess and asks the user to pin one. Picking a
number nobody else uses is a kindness.

### `detect` — which projects this claims

```kpl
detect {
  file_exists "build.zig"
  file_exists_any ["build.zig.zon", "zig.mod"]
  dir_exists ".zig-cache"
  file_contains { path: "build.zig", pattern: "addExecutable" }
}
```

Rules are **or**-ed: any one matching claims the directory. Each rule that fires
adds to the reported *score*, which is diagnostic detail — ranking is by
`priority`.

Keep these cheap. They run for every installed plugin on every invocation, so
they should be stat calls, not file reads, wherever possible. Paths are
sandboxed: an absolute path or one containing `..` simply never matches.

### `requires` — the tools your commands need

```kpl
requires {
  any_of   [zig]                 // at least one of these must exist
  optional [zls, "zig-fmt-check"] // nice to have; kap reports but does not fail
}
```

`kap doctor` reads this. `any_of` means *one of these is enough* — a name with a
dash has to be quoted, because a bare word must be an identifier.

### `schema` — what users can configure

```kpl
schema {
  build_step:  str       = "install"
  release:     bool      = false
  optimize:    enum { debug, safe, fast, small } = debug
  extra_args:  list<str> = []
  jobs:        int       = 0
}
```

Every field needs a type and a default. kap validates a user's `kap.toml`
against this *before* your code runs, so a typo produces a clear error instead
of a confusing command line.

Types: `str`, `int`, `bool`, `list<str>`, `list<int>`, `enum { a, b, c }`.

> **`none` is not a usable enum member.** `none` is KPL's absent-value literal,
> so `none => ...` in a `match` reads as that literal rather than as your
> member. kap refuses the declaration and suggests a rename; `off` is the
> conventional choice.

### `command` — what each kap command runs

```kpl
command build(project, config, extra) {
  let cmd = ["zig", "build", config.build_step]
  if config.release { cmd = cmd + ["-Doptimize=ReleaseSafe"] }
  step cmd + config.extra_args + extra
}
```

The parameters are fixed: `project` (what kap knows about the directory),
`config` (the merged configuration, matching your schema), and `extra`
(everything the user put after `--`). A command that ignores `extra` can leave
it off.

The names kap dispatches: `build`, `check`, `ci`, `clean`, `dev`, `doctor`,
`fmt`, `install`, `lint`, `ports`, `run`, `test`. Define the ones that make
sense; a command you do not define is reported clearly rather than silently
doing nothing.

---

## Writing commands

A command builds up a list of steps. It never runs anything.

```kpl
command build(project, config, extra) {
  step mkdir "-p" config.build_dir              // bare words: a program and its args
  step ["zig", "build"] + extra                 // a list expression
  step { cmd: ["zig", "test"], cwd: "tests", label: "tests" }   // the record form
}
```

Three forms, and the reason for each:

- **Bare words** read naturally for a fixed command: `step rm "-rf" dir`. An
  identifier here is a variable if one is bound and the literal word otherwise,
  which is what lets `mkdir` and `dir` both work in `step mkdir "-p" dir`.
- **A list expression** is what you want when the argv is built up: it is the
  only form that can take `+ extra`.
- **The record form** is the only way to set `cwd`, `env`, or `label`.

Two block-level modifiers:

```kpl
concurrent true        // run every step at once, with labelled output (kap dev)
report_freed_space     // print how much space was recovered (kap clean)
```

---

## What a command can see

| Expression | Type | What it does |
|---|---|---|
| `project.root` | `str` | Absolute path to the project |
| `project.matched_files` | `list<str>` | Which detect markers fired |
| `project.exists(p)` | `bool` | Does this path exist, under the root |
| `project.read(p)` | `str` | Read a file — capped at 1 MiB, sandboxed to the root |
| `project.glob(p)` | `list<str>` | Match a pattern — capped at 10 000 results |
| `project.tool(n)` | `bool` | Is this program on `PATH` (checked, never run) |
| `project.env(n)` | `str?` | An environment variable, with secrets filtered out |
| `len(list)` | `int` | |
| `contains(s, sub)` | `bool` | |
| `trim(s)` | `str` | |
| `split(s, sep)` | `list<str>` | |

That is the whole surface. There is no way to open an arbitrary file, run a
program, or reach the network — see [what plugins cannot do](#what-plugins-cannot-do).

---

## Testing without running anything

A test case is one JSON file. kap evaluates your command block against a
fixture directory and compares the resulting command list against it. **Nothing
is executed** — which is why kap's own CI tests all six bundled plugins with no
cmake, cargo, npm, go, or uv installed anywhere.

```
tests/
├── fixtures/simple-project/       a fake project: whatever your detect rules and
│   └── build.zig                  project.read/glob/exists calls need to find
└── expected/
    ├── simple-project.build.steps.json
    └── simple-project.build-release.steps.json
```

The file name binds the two halves: `<fixture>.<command>.steps.json`. The last
dotted component is the command, so a fixture name may itself contain dots.

```json
{
  "config": { "release": true },
  "extra":  ["--summary", "all"],
  "tools":  ["zig"],
  "env":    { "CI": "true" },

  "steps": [
    { "cmd": ["zig", "build", "install", "-Doptimize=ReleaseSafe", "--summary", "all"] }
  ]
}
```

| Field | Meaning |
|---|---|
| `fixture` | Which fixture directory — defaults to the name before the last dot |
| `command` | Which command — defaults to the last dotted component |
| `config` | Overrides on your schema defaults |
| `extra` | What the user put after `--` |
| `tools` | What `project.tool()` reports as installed |
| `env` | What `project.env()` returns |
| `steps` | The command list you expect |
| `concurrent` | Expect `concurrent true` |
| `report_freed_space` | Expect `report_freed_space` |

`tools` and `env` are **declared, not read from the machine**. A test that
consulted the real `PATH` would pass on your laptop and fail in CI. Everything
else — `exists`, `read`, `glob` — comes from the fixture directory through the
normal sandbox, which is what a fixture is for.

Several cases can share one command; the name before `.<command>` distinguishes
them, and `"command"` in the JSON says which block to run:

```json
{ "command": "build", "config": { "release": true }, "steps": [ ... ] }
```

Omitted step fields (`cwd`, `env`, `label`) and spec fields (`concurrent`,
`report_freed_space`) take their defaults, so a golden file only spells out
what it cares about.

### Reading a failure

```console
$ kap plugin test zig
[FAIL] zig simple-project.build
      steps differ
      expected: {"concurrent":false,"report_freed_space":false,"steps":[{"cmd":["zig","build"], ...
      actual:   {"concurrent":false,"report_freed_space":false,"steps":[{"cmd":["zig","biuld"], ...
```

Both renderings are shown in full, because "it differs" is not actionable.

---

## The development loop

```sh
kap plugin new my-thing         # scaffold
$EDITOR my-thing/plugin.kpl     # write it
kap plugin doctor my-thing      # does it parse, validate, and type-check?
kap plugin test my-thing        # do its cases pass?
kap plugin install --link my-thing
cd ~/code/a-real-project
kap build -n                    # what would it actually do?
```

`--link` installs a **symlink**, so every edit takes effect on the next `kap`
run with no reinstall. `kap plugin remove` later removes the link and leaves
your working copy alone.

`kap plugin doctor` and `kap plugin test` both take a *path*, so you can check a
plugin before installing anything. That is deliberate: needing to install
something before you can syntax-check it would be backwards.

Useful while iterating:

```sh
kap build -n --verbose          # the steps, plus which plugin and config were used
kap detect --refresh            # re-run detection, ignoring the cache
kap build --set key=value       # try a config value without editing a file
```

---

## Installing and sharing

```sh
kap plugin install --link ./my-thing         # development: a symlink
kap plugin install ./my-thing                # a copy, for yourself
kap plugin install https://github.com/you/kap-zig    # straight from a repository
kap plugin install --project ./my-thing      # into ./.kap/plugins, committed with the repo
kap plugin install zig                       # from the registry, once it is listed
```

`--project` is the one to know for a team: it puts the plugin in
`./.kap/plugins/`, which is committed with the code, so everyone who clones the
repository gets it without installing anything. Project-local plugins take
precedence over installed ones of the same name.

Before writing anything, kap checks that the payload parses, that the manifest
is complete, that `api_version` is one it supports, that the detect rules are
well-formed, and that every command type-checks. Then it shows a summary and
asks. A non-interactive stdin is treated as **no**; a script that means it
passes `--yes`.

---

## Publishing to a registry

A registry is a git repository with an `index.toml`. No server.

```toml
[plugins.zig]
description = "Build and test Zig projects"
version = "1.0.0"
url = "https://github.com/you/kap-plugins"
ref = "v1.0.0"
subdir = "zig"
checksum = "sha256:…"
tags = ["zig", "build"]
```

`checksum` is SHA-256 over the plugin's files — every regular file under the
directory in sorted path order, with the paths themselves folded in and `.git`
excluded. It is **enforced**: a payload whose digest does not match causes the
install to fail, not to warn.

To find the digest, install once without a checksum and read it back:

```console
$ kap plugin install ./zig --yes
$ grep checksum ~/.local/share/kap/installed-plugins.toml
checksum = "sha256:8cc58f2f…"
```

Point kap at your registry with `KAP_REGISTRY=/path/to/index.toml`, and users
find it with `kap plugin search`.

A bundle installs several at once:

```toml
[bundles.zig-all]
description = "Zig and its friends"
plugins = ["zig", "zls"]
```

What the checksum does and does not buy, stated plainly: it proves the payload
is what the index author recorded, so a compromised mirror or a truncated
download is caught. It says nothing about whether the plugin is *safe*. That is
why installing shows a summary and asks.

---

## What plugins cannot do

A plugin is third-party code fetched from a git repository, so kap treats it as
untrusted. The limits are structural, not policy:

- **No process execution.** `step` appends to a list. Only kap's executor spawns
  anything, which is what makes `--dry-run` complete rather than best-effort.
- **No filesystem access beyond `project.exists/read/glob`.** Every path is
  canonicalised and refused if it leaves the project root. This applies to
  detect rules too, which run before the interpreter exists.
- **Bounded reads.** `project.read` stops at 1 MiB; `project.glob` at 10 000
  results.
- **Filtered environment.** `project.env` denies `*_TOKEN`, `*_KEY`,
  `*_SECRET`, `*_PASSWORD`, `AWS_*`, and friends.
- **No imports, no network, no randomness, no clock.** The same inputs always
  produce the same command list — which is what makes golden-file tests
  meaningful.

If you find yourself wanting to escape one of these, the answer is usually a
`step` that runs a tool which can do it.

---

## Recipes

### Choose a tool based on what is installed

```kpl
let generator = match config.generator {
  auto           => if project.tool("ninja") then "Ninja" else none,
  ninja          => "Ninja",
  make           => "Unix Makefiles",
  unix_makefiles => "Unix Makefiles",
}
if generator != none {
  cmd = cmd + ["-G", generator]
}
```

`none` is how a KPL expression says "no value", and a flag built from it is
simply not added.

### Choose a package manager from the lockfile

```kpl
let pm = match config.package_manager {
  auto => if project.exists("pnpm-lock.yaml") then "pnpm"
          else if project.exists("yarn.lock") then "yarn"
          else "npm",
  npm  => "npm",
  pnpm => "pnpm",
  yarn => "yarn",
}
```

The lockfile describes the *repository*; what is installed on the machine
describes the machine. For this question the first one is right.

### Run every workspace at once

```kpl
command dev(project, config) {
  let manifest_text = project.read("package.json")
  if contains(manifest_text, "\"workspaces\"") {
    concurrent true
    for ws in project.glob("packages/*") {
      if project.exists(ws + "/package.json") {
        step { cmd: ["npm", "run", "dev"], cwd: ws, label: ws }
      }
    }
  } else {
    step ["npm", "run", "dev"]
  }
}
```

### Verify formatting in CI, rewrite it locally

```kpl
schema { check: bool = false }

command fmt(project, config, extra) {
  let args = if config.check then ["--", "--check"] else []
  step ["cargo", "fmt"] + args + extra
}
```

`kap ci` sets `check` for you when your schema declares it as a bool — that is
how "fmt-check" happens without a separate command.

### Report what a clean recovered

```kpl
command clean(project, config) {
  step rm "-rf" config.build_dir
  report_freed_space
}
```

### An optional flag from a string setting

```kpl
schema { jobs: str = "" }        // str, not int: `-j 8` needs the 8 as a word

command build(project, config, extra) {
  let cmd = ["make"]
  if config.jobs != "" { cmd = cmd + ["-j", config.jobs] }
  step cmd + extra
}
```

KPL has no integer-to-string conversion, which is why a numeric flag is
configured as a string.

---

## Mistakes worth avoiding

**Forgetting `+ extra`.** Without it, `kap build -- --verbose` silently drops
the user's argument. Add `extra` to the step a user would expect it on — for a
configure-then-build plugin, that is the build step, not the configure step.

**Reading files in `detect`.** Detect rules run for every installed plugin on
every invocation. `file_exists` is a stat; `file_contains` is a read. Prefer the
first, and reach for the second only when the file name alone is ambiguous.

**Picking a priority that collides.** A tie makes kap refuse to run until the
user pins one. Check the bundled numbers (10/30/35/38/40/45) and choose one
nobody else uses.

**Guessing on the user's behalf.** If your ecosystem cannot say which binary
`kap run` should execute, make it a schema field and let the user say. A wrong
guess is worse than a clear question — `cmake-cpp` requires `run_target` for
exactly this reason.

**Cleaning too much.** `node_modules` and `.venv` are technically build output.
Deleting them turns a five-second clean into a five-minute reinstall. Leave them
out of the default and let a user who wants that add them.

**An enum member called `none`.** It cannot be matched on. kap refuses the
schema and tells you; `off` is the usual replacement.

**Forgetting the README.** A plugin's configuration keys are discoverable only
if you write them down. Every bundled plugin has a README with a table of every
key, its type, its default, and what it means — copy that shape.
