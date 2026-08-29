# doctor

Checks that the tools this project's plugins need are actually installed.

One of the two bundled system plugins (design doc §6.6). It is written in KPL,
not C++, because §4 makes a claim the codebase has to back up: the DSL is
expressive enough for system introspection, not only for shelling out to a
build tool.

## How it knows what to check

KPL has no way to see other plugins. That is a deliberate sandbox property —
§7 treats plugins as untrusted code, and a plugin that could enumerate its
neighbours would be a plugin that could fingerprint your machine.

So the core does the one thing only it can: it reads the `requires` block of
every plugin that matched this directory and injects the result into this
plugin's `config` record. Everything after that — what to check, what counts as
healthy, what to print, what exit status to produce — is decided here, in KPL.

The injected fields are ordinary schema fields with empty-list defaults, so this
plugin still type-checks and still runs standalone. That is how
`kap plugin test doctor` exercises it without any of the machinery above.

### `any_of` groups

`required_tools` carries one element per `any_of` group, comma-joined:

| Plugin's `requires` | Injected element |
|---|---|
| `any_of [cmake]` | `"cmake"` |
| `any_of [ss, lsof, netstat]` | `"ss,lsof,netstat"` |

The plugin splits each group with the `split` builtin and reports the group as
satisfied when **any one** of its members is installed. Flattening them would
report a machine that has `ss` as missing `lsof` and `netstat`, which is exactly
backwards.

## Output

```
kap doctor
  plugin   cmake-cpp
  plugin   doctor
  plugin   ports
  ok       cmake
  ok       ss
  --       ccache  (optional, not installed)
  ok       ninja  (optional)
healthy
```

Exit status is 0 when healthy and 1 when a required tool is missing — so
`kap doctor` is usable as a CI gate, not just as something to read. A missing
*optional* tool is reported but does not fail: `ccache` not being installed is
a slower build, not a broken one. Set `strict = true` to make it fail anyway.

## Configuration

Under `[plugins.doctor]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `required_tools` | list of strings | `[]` | Injected by the core; one comma-joined `any_of` group per element |
| `optional_tools` | list of strings | `[]` | Injected by the core |
| `matched_plugins` | list of strings | `[]` | Injected by the core |
| `strict` | bool | `false` | Make a missing optional tool a failure too |

The injected values sit at the bottom of the configuration chain (§5.12), so
both `kap.toml` and `--set` override them — which is how you test a scenario:

```sh
kap doctor --set required_tools=cmake,ninja
```

One wrinkle: `--set` splits a list value on commas, so the line above means
*two groups of one*, not one group of two alternatives. A group has to arrive
as a single string, which only a TOML array element can be:

```toml
[plugins.doctor]
required_tools = ["ss,lsof,netstat"]
```

## Detection

Composable (§3.3) with priority 1, and its `detect` block matches every
directory. It rides alongside whichever plugin actually owns the project and
never competes to answer `kap build`.

## Tests

```sh
kap plugin test doctor
```
