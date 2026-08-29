# KPL — the Kap Plugin Language

The complete language reference. For a guided introduction, start with
[plugins.md](plugins.md).

KPL is the only way to extend kap. There is no C++ plugin ABI, no embedded Lua
or JavaScript, and no dynamic library loading — a plugin is text that the core
parses and interprets. It is deliberately small: no user-defined functions, no
loops over anything but a list, no I/O beyond a handful of read-only builtins.
That is what makes a plugin fetched from a git repository safe to run and
possible to audit in a sitting.

---

## Contents

- [File structure](#file-structure)
- [Lexical rules](#lexical-rules)
- [`manifest`](#manifest)
- [`detect`](#detect)
- [`requires`](#requires)
- [`schema`](#schema)
- [`command`](#command)
- [Types](#types)
- [Expressions](#expressions)
- [Statements](#statements)
- [`step`](#step)
- [Modifiers](#modifiers)
- [Builtins](#builtins)
- [The `CommandSpec` contract](#the-commandspec-contract)
- [Type checking](#type-checking)
- [Sandbox and limits](#sandbox-and-limits)
- [Versioning](#versioning)
- [Grammar](#grammar)

---

## File structure

One file, `plugin.kpl`, holding up to five kinds of top-level block:

```kpl
manifest { ... }        // identity and ranking            (required)
detect   { ... }        // which projects this claims      (optional)
requires { ... }        // tools the commands need         (optional)
schema   { ... }        // what users may configure        (optional)
command name(...) { }   // one per kap command             (any number)
```

Order does not matter. A plugin with no `detect` block never matches anything;
one with no `schema` declares no configuration surface, and its `config` reads
go unchecked rather than being reported as unknown keys.

---

## Lexical rules

| | |
|---|---|
| Identifiers | `[A-Za-z_][A-Za-z0-9_]*` |
| Strings | `"..."` with the escapes `\"`, `\\`, `\n`, `\t` |
| Integers | 64-bit signed, decimal |
| Booleans | `true`, `false` |
| Absent value | `none` |
| Comments | `//` to end of line |
| Whitespace | Insignificant, including newlines |

There are no keywords reserved outside their position: `build` is a fine
variable name, and `step` is only special at the start of a statement.

`\uXXXX` is not supported. Every string a plugin produces is a path or an argv
word, and correct `\u` handling needs UTF-16 surrogate pairing to be worth
having at all.

---

## `manifest`

Assignments only. Unknown keys are ignored, so a manifest written for a later
kap still loads.

```kpl
manifest {
  name        = "cmake-cpp"   // str, required. What users type and configure under
  version     = "1.2.0"       // str, required. Semver by convention
  api_version = 1             // int, required. See "Versioning" below
  priority    = 30            // int, default 0. Higher wins a contested directory
  composable  = false         // bool, default false. See below
  supersedes  = []            // list<str>, default []. Plugins to ignore when this matches
}
```

**`priority`** is the only thing that ranks plugins. The bundled ones:
`make-generic` 10, `cmake-cpp` 30, `node` 35, `python-uv` 38, `cargo-rust` 40,
`go` 45, and the system plugins at 1. A tie between two non-composable plugins
is a hard error asking the user to pin one — never a silent guess.

**`composable = true`** means "run alongside whichever plugin owns this
directory" rather than "compete to be it". A composable plugin never becomes the
primary; it contributes any command the primary does not define. `doctor` and
`ports` work this way, which is how `kap doctor` works in every directory.

**`supersedes`** removes the named plugins from the match set entirely whenever
this one matches, regardless of their priority.

---

## `detect`

Declarations, not statements. Rules are **or**-ed: any one matching claims the
directory. Each rule that fires adds one to the reported *score*.

```kpl
detect {
  file_exists "CMakeLists.txt"
  file_exists_any ["*.xcworkspace", "*.xcodeproj"]
  dir_exists ".git/modules"
  file_contains { path: "package.json", pattern: "\"workspaces\"" }
}
```

| Rule | Argument | Matches when |
|---|---|---|
| `file_exists` | one path string | the path exists |
| `file_exists_any` | a list of path strings | any of them exists |
| `dir_exists` | one path string | the path exists **and is a directory** |
| `file_contains` | `{ path: str, pattern: str }` | the file exists and contains the substring |

`file_exists` and `file_exists_any` accept a wildcard in the **final path
component** — `*` matches any run of characters, `?` exactly one.
`file_contains` is a plain substring test, not a regular expression.

Two things to keep in mind:

- **These are the hot path.** They run for every installed plugin on every
  invocation. Prefer `file_exists` (a stat) over `file_contains` (a read).
- **They are sandboxed.** An absolute path, or one that escapes the project root
  with `..`, never matches. This is enforced in the detection engine itself,
  before any interpreter exists, so a plugin cannot map the filesystem above a
  project by watching which plugin wins.

An unrecognised rule is a **load-time error**, not a silent skip: a plugin that
never matches with nothing to explain why is the worst possible outcome for its
author.

---

## `requires`

What `kap doctor` reports on. Declarations, like `detect`.

```kpl
requires {
  any_of   [cmake]                        // at least one of these must be installed
  optional [ninja, make, ccache]          // reported if absent, but not a failure
  optional [gofumpt, "golangci-lint"]     // quote a name that is not an identifier
}
```

`any_of` is a *group*: one member being installed satisfies the whole
requirement. Bare words inside a list are read as strings, which is why
`any_of [cmake]` needs no quotes — but `golangci-lint` contains a dash and must
be quoted.

An unrecognised directive here is ignored rather than fatal; `api_version` is
what guards against real incompatibility.

---

## `schema`

Every field needs a type and a default. kap validates a user's configuration
against this **before** any of the plugin runs.

```kpl
schema {
  generator:   enum { auto, ninja, make } = auto
  build_dir:   str       = "build"
  jobs:        int       = 1
  release:     bool      = false
  cmake_args:  list<str> = []
  levels:      list<int> = []
}
```

| Type | Literal form | Notes |
|---|---|---|
| `str` | `"hello"` | UTF-8, immutable |
| `int` | `42` | 64-bit signed |
| `bool` | `true` / `false` | |
| `list<str>` | `["a", "b"]` | Homogeneous |
| `list<int>` | `[1, 2]` | Homogeneous |
| `enum { a, b }` | a bare member name | Closed set, validated at merge time |

A field's declared type is what the type checker uses inside a command block, so
`config.cmake_args` really is a `list<str>` and `["cmake"] + config.cmake_args`
type-checks. A `match` over an enum-typed field is checked for exhaustiveness.

> **`none` cannot be an enum member.** `none` is the absent-value literal, so
> `none => ...` in a match arm reads as that literal rather than as your member,
> and the exhaustiveness check then reports the member as uncovered. kap refuses
> the declaration and suggests a rename. Use `off`.

---

## `command`

```kpl
command build(project, config, extra) {
  ...
}
```

Parameters are positional and fixed:

| Name | What it is |
|---|---|
| `project` | The host object — see [Builtins](#builtins) |
| `config` | The merged configuration, matching your `schema` |
| `extra` | `list<str>` — everything the user put after `--` |

A command that does not use `extra` may declare only `(project, config)`.

Command names kap dispatches: `build`, `check`, `ci`, `clean`, `dev`, `doctor`,
`fmt`, `install`, `lint`, `ports`, `run`, `test`. Others are permitted but
unreachable from the CLI.

---

## Types

```
str  int  bool  none  list<T>  enum  record  project
```

`none` is the absent value. It compares equal only to itself, and it is how an
expression says "no flag here":

```kpl
let generator = if project.tool("ninja") then "Ninja" else none
if generator != none { cmd = cmd + ["-G", generator] }
```

`record` is the `{ key: value }` form. Its only run-time use is the `step`
record form and `detect`'s `file_contains`.

---

## Expressions

### Operators, loosest binding first

| Operator | Meaning |
|---|---|
| `\|\|` | Logical or (short-circuits) |
| `&&` | Logical and (short-circuits) |
| `==` `!=` | Equality |
| `<` `>` `<=` `>=` | Ordering, on integers |
| `+` `-` | Addition; `+` also concatenates strings and lists |
| `*` `/` | Multiplication, division |
| `!` `-` (unary) | Negation |
| `.` `()` `[]` | Member access, call, index |

`+` is overloaded on purpose and only three ways: `int + int`, `str + str`, and
`list<T> + list<T>`. Element types survive concatenation, so
`["cmake"] + config.cmake_args` is a `list<str>`.

### Literals

```kpl
"a string"      42      true      false      none
["a", "b"]                      // list
{ cmd: ["x"], cwd: "sub" }      // record
```

### Inline conditional

```kpl
let flag = if config.release then "--release" else none
```

Both branches are expressions. Nesting in the `else` is how you write a chain:

```kpl
let pm = if project.exists("pnpm-lock.yaml") then "pnpm"
         else if project.exists("yarn.lock") then "yarn"
         else "npm"
```

### `match`

An expression, so it can be assigned:

```kpl
let generator = match config.generator {
  auto           => if project.tool("ninja") then "Ninja" else none,
  ninja          => "Ninja",
  make           => "Unix Makefiles",
  unix_makefiles => "Unix Makefiles",
}
```

A bare identifier pattern is an **enum member compared as its own text**, never
a variable read: `ninja => "Ninja"` binds nothing.

There is no catch-all pattern in v1. Over an enum, the checker proves every
member is covered; over a `str`, an uncovered value is a located run-time error.
That is the honest outcome when a member is added and an arm is forgotten.

---

## Statements

```kpl
let name = expr                 // bind
name = expr                     // reassign an existing binding
step ...                        // append a step; see below
if expr { ... } else if expr { ... } else { ... }
for name in list_expr { ... }
match expr { ... }              // in statement position, the value is discarded
concurrent true
report_freed_space
fail expr                       // stop the command and tell the user why
```

`for` iterates lists only — there is no numeric range form. The lists it can
iterate all come from the host and are already capped.

Scoping is flat: a `for` loop's body can assign to a binding made outside it,
which is how an accumulator works.

```kpl
let missing = []
for tool in config.required_tools {
  if !project.tool(tool) { missing = missing + [tool] }
}
if len(missing) > 0 { step ["false"] }
```

---

## `step`

Three forms.

### Variadic — bare words and expressions

```kpl
step mkdir "-p" dir
step rm "-rf" config.build_dir
```

An identifier standing alone as a step argument resolves to a **variable if one
is bound, and to the literal word otherwise**. That rule is what lets `mkdir`
(a program) and `dir` (a variable) both work unquoted in the same line.

It applies *only* in that position. `step [name]` and `step name + ".txt"` still
require a real binding, so the cost — a mistyped variable becoming a bare word —
is confined to the one place the syntax needs it.

A string contributes one word; a list splices in all of its words. `step "ninja" dir`
and `step ["ninja", dir]` produce the same argv.

### List expression

```kpl
step ["cargo", "build"] + flags + extra
```

The form to use whenever the argv is built up, and the only one that can
concatenate.

### Record

```kpl
step { cmd: ["npm", "run", "dev"], cwd: "packages/app", label: "app" }
step { cmd: ["cargo", "test"], env: { RUST_LOG: "debug" } }
```

The only way to set a step's `cwd`, `env`, or `label`. Mixing it with other
arguments is an error, and an unrecognised field is rejected rather than
ignored.

| Field | Type | Meaning |
|---|---|---|
| `cmd` | `list<str>` | The argv array. Required |
| `cwd` | `str` | Working directory, relative to the project root |
| `env` | record of `str` | Variables added for this step only |
| `label` | `str` | Prefix for this step's output in concurrent mode |

---

## Modifiers

```kpl
concurrent true        // the executor runs every step at once
report_freed_space     // print how much space the command recovered
```

`concurrent` changes how the whole spec runs, not one step: every step starts
together, output is prefixed with each step's label, and all of them finish
before kap reports the first failure. This is what `kap dev` uses for a
workspace.

`report_freed_space` measures the paths named in the steps' arguments before and
after, and prints the difference. A command with no path-shaped argument falls
back to measuring the whole project root.

---

## `fail`

```kpl
fail "package.json declares no \"build\" script"
```

`fail` ends the command. Nothing after it runs, at any nesting depth, and the
executor prints the message as `kap: error: <message>` and exits non-zero
without spawning a single process. Steps accumulated before the `fail` are kept
in the spec so `--dry-run` can show how far the plan got, but they are never
executed.

The operand must be a `str`, checked at load time. Nothing is coerced: a
non-string message is a plugin bug, and stringifying it would put the
interpreter's idea of a value in front of a user who cannot act on it.

Use it when the plugin can tell, from the project itself, that the command it
is about to build cannot possibly work — a missing npm script, a config field
that has to be set and is not. This is the difference between:

```
npm error Missing script: "start"
npm error Did you mean one of these?
```

and

```
kap: error: package.json declares no "start" script, which is what 'kap run'
runs; this project has a "dev" script — try 'kap dev'
```

Both are true. Only the second one knows it was `kap run` that picked the name
`start`, that `start_script` in kap.toml can change it, and that a sibling kap
command already does what the user wanted.

`fail` is **not** the channel for a broken plugin. A malformed `step`, an
unknown identifier, or a type error is a `diag::Error` addressed to the plugin's
*author*, and carries a source location. A `fail` is the plugin working
correctly and saying something true about the project it was pointed at, so it
carries no location: `plugin.kpl:127` in front of someone whose actual problem
is a missing npm script is noise.

---

## Builtins

### `project`

| Expression | Type | Description |
|---|---|---|
| `project.root` | `str` | Absolute path to the project root |
| `project.matched_files` | `list<str>` | The detect markers that fired |
| `project.exists(path)` | `bool` | File or directory exists under the root |
| `project.read(path)` | `str` | Read a file, capped at 1 MiB |
| `project.glob(pattern)` | `list<str>` | Paths relative to the root, capped at 10 000, sorted |
| `project.tool(name)` | `bool` | Executable on `PATH` — checked with `access(X_OK)`, never run |
| `project.env(name)` | `str?` | Environment variable, deny-listed |

`project.glob` expands a wildcard in the **final path component** only, and
returns results sorted so a plugin's step list is stable.

### Free functions

| Function | Signature |
|---|---|
| `len` | `(list) -> int` |
| `contains` | `(haystack: str, needle: str) -> bool` |
| `trim` | `(s: str) -> str` |
| `split` | `(s: str, sep: str) -> list<str>` |

There are no user-defined functions in v1. That is a real cost — the `node`
plugin repeats its package-manager `match` in every command — and it is the
price of an interpreter small enough to audit.

---

## The `CommandSpec` contract

A command block returns nothing explicitly. It accumulates steps, and kap builds:

```json
{
  "steps": [
    {
      "cmd": ["cmake", "-S", ".", "-B", "build"],
      "cwd": null,
      "env": {},
      "label": null
    }
  ],
  "concurrent": false,
  "report_freed_space": false,
  "failure": null
}
```

`failure` is null unless a `fail` statement ran, in which case it holds the
message and `steps` holds whatever the command had accumulated before stopping.
A golden file asserts one like any other field:

```json
{
  "command": "run",
  "failure": "package.json declares no \"start\" script, which is what 'kap run' runs",
  "steps": []
}
```

This is exactly the shape a `tests/expected/*.steps.json` golden file is written
in. When kap writes one it fills in every field, so two specs that behave
identically serialise identically and can be diffed as text; when it reads one,
the optional fields are optional.

---

## Type checking

A plugin is checked at **load time**, before any of it runs. `kap plugin doctor`
runs the same check.

What is caught:

- A `config.<key>` that is not in the `schema` block.
- An operator applied to the wrong types (`"a" + 1`).
- A `match` over an enum-typed field that does not cover every member.
- A `step` whose argument cannot be an argv array.
- A record field the `step` form does not recognise.
- A schema field with no default, or a default of the wrong type.
- An enum default naming a member the enum does not declare.

What is not: anything that depends on run-time values. `project.read` on a
missing file is a located run-time error, because whether the file exists is not
knowable at load time.

Values whose type genuinely cannot be known statically — a plugin with no
`schema` block reading `config.anything` — are permitted rather than guessed at.

---

## Sandbox and limits

| | |
|---|---|
| Process execution | None. `step` appends to a list; only kap's executor spawns |
| Filesystem | `project.exists`, `read`, `glob` only. Paths canonicalised, escapes refused |
| Read size | 1 MiB per `project.read` |
| Glob results | 10 000 |
| Environment | Deny-listed: `*_TOKEN`, `*_KEY`, `*_SECRET`, `*_PASSWORD`, `AWS_*`, … |
| Network | None |
| Imports | None in v1. One file per plugin |
| Randomness, time | None. The same inputs always produce the same spec |

That last line is what makes golden-file testing meaningful: a plugin cannot
produce a different answer on a different day.

---

## Versioning

`api_version` in the manifest declares which KPL a plugin was written for. This
kap supports **1**.

- A plugin declaring a version this kap does not support is a **hard error**
  wherever you named it explicitly — `kap plugin install`, `doctor`, `test`.
- During **detection** it is skipped with a warning, so one too-new plugin
  cannot break `kap build` in an unrelated project.

Adding a builtin, a statement, or a rule is additive and does not need a bump.
Changing what existing syntax means does.

---

## Grammar

```ebnf
file        = { top_level_decl } ;

top_level_decl
            = manifest_block | detect_block | requires_block
            | schema_block | command_decl ;

manifest_block = "manifest" block ;
detect_block   = "detect" directive_block ;
requires_block = "requires" directive_block ;
schema_block   = "schema" block ;

(* `detect` and `requires` hold declarations, not statements. A directive
   argument is deliberately never a bare identifier, so an identifier at
   argument position unambiguously starts the next directive — no lookahead,
   no newline sensitivity. Bare identifiers *inside* a list are read as
   strings, which is why `any_of [cmake]` needs no quotes. *)
directive_block = "{" { directive } "}" ;
directive       = IDENT { directive_arg } ;
directive_arg   = STRING | INT | directive_list | directive_record ;
directive_list  = "[" [ directive_elem { "," directive_elem } ] "]" ;
directive_record= "{" field_lit { "," field_lit } "}" ;
field_lit       = IDENT ":" directive_elem ;
directive_elem  = STRING | INT | BOOL | IDENT ;

command_decl   = "command" IDENT "(" param_list ")" block ;

block       = "{" { stmt } "}" ;

stmt        = let_stmt | assign_stmt | step_stmt | if_stmt | for_stmt
            | match_stmt | concurrent_stmt | report_stmt | fail_stmt ;

let_stmt    = "let" IDENT "=" expr ;
step_stmt   = "step" step_arg { step_arg } | "step" expr ;
if_stmt     = "if" expr block { "else" "if" expr block } [ "else" block ] ;
for_stmt    = "for" IDENT "in" expr block ;

(* `match` is an EXPRESSION; match_stmt is one in statement position, whose
   value is discarded. There is no catch-all pattern in v1 — an uncovered value
   is a located run-time error, which is the honest outcome when an enum member
   is added and an arm is forgotten. *)
match_expr  = "match" expr "{" match_arm { match_arm } "}" ;
match_stmt  = match_expr ;
match_arm   = pattern "=>" expr [ "," ] ;
assign_stmt = IDENT "=" expr ;

concurrent_stmt = "concurrent" BOOL ;
report_stmt     = "report_freed_space" ;
fail_stmt       = "fail" expr ;

step_arg    = STRING | expr ;

param_list  = [ param { "," param } ] ;
param       = IDENT ;

expr        = assign_expr ;
assign_expr = or_expr ;
or_expr     = and_expr { "||" and_expr } ;
and_expr    = eq_expr { "&&" eq_expr } ;
eq_expr     = cmp_expr { ( "==" | "!=" ) cmp_expr } ;
cmp_expr    = add_expr { ( "<" | ">" | "<=" | ">=" ) add_expr } ;
add_expr    = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr    = unary_expr { ( "*" | "/" ) unary_expr } ;
unary_expr  = ( "!" | "-" ) unary_expr | postfix_expr ;
postfix_expr= primary_expr { "." IDENT | "(" arg_list ")" | "[" expr "]" } ;
primary_expr= literal | IDENT | "(" expr ")" | list_expr | record_expr
            | match_expr
            | "if" expr "then" expr "else" expr ;

(* A bare IDENT pattern is an enum MEMBER compared as its own text, not a
   variable read: `ninja => "Ninja"` never binds anything. *)
pattern     = literal | IDENT | "none" ;

literal     = STRING | INT | BOOL | "none" ;
list_expr   = "[" [ expr { "," expr } ] "]" ;
record_expr = "{" field { "," field } "}" ;
field       = IDENT ":" expr ;
arg_list    = [ expr { "," expr } ] ;
```

---

## A complete plugin

```kpl
// cmake-cpp — build, test, and clean a CMake project.

manifest {
  name        = "cmake-cpp"
  version     = "1.0.0"
  api_version = 1
  priority    = 30
  composable  = false
  supersedes  = []
}

detect {
  file_exists "CMakeLists.txt"
}

requires {
  any_of   [cmake]
  optional [ninja, make, ccache]
}

schema {
  generator:  enum { auto, ninja, make, unix_makefiles } = auto
  build_dir:  str       = "build"
  build_type: str       = "Debug"
  cmake_args: list<str> = []
}

command build(project, config, extra) {
  let dir = config.build_dir
  step mkdir "-p" dir

  let configure = ["cmake", "-S", ".", "-B", dir]

  let generator = match config.generator {
    auto           => if project.tool("ninja") then "Ninja" else none,
    ninja          => "Ninja",
    make           => "Unix Makefiles",
    unix_makefiles => "Unix Makefiles",
  }
  if generator != none {
    configure = configure + ["-G", generator]
  }

  configure = configure + ["-DCMAKE_BUILD_TYPE=" + config.build_type]
  configure = configure + config.cmake_args
  step configure

  step ["cmake", "--build", dir] + extra
}

command test(project, config, extra) {
  step ["ctest", "--test-dir", config.build_dir, "--output-on-failure"] + extra
}

command clean(project, config) {
  step rm "-rf" config.build_dir
  report_freed_space
}
```

The real thing, with more commands, is
[kap-plugins/cmake-cpp/plugin.kpl](../kap-plugins/cmake-cpp/plugin.kpl). All eight
bundled plugins are written to be read.
