# ports

Shows which local ports are listening and what is holding them.

The second bundled system plugin (design doc §6.6). Like `doctor`, it is written
in KPL to demonstrate §4's claim that the DSL reaches system introspection.

## Why it shells out instead of reading `/proc/net/tcp`

Milestone 9 describes this plugin as "reads `/proc/net/tcp` (Linux) or lsof
fallback via step". The first half cannot be done from KPL, for two independent
reasons — and both are features rather than gaps:

1. **The sandbox.** §7 canonicalises every path a plugin passes to
   `project.read` and refuses anything outside the project root. `/proc` is
   outside every project root. Special-casing it would put a hole in the one
   rule that makes plugins safe to install from a git URL.

2. **The type system.** `/proc/net/tcp` stores addresses and ports as
   big-endian hex (`0100007F:1F90`), and KPL has no hex parsing, no integer
   formatting, and no bit operations — deliberately (§5.6). A plugin language
   that could decode it would be a much larger thing to audit.

So this takes the second half of that sentence: a step. `ss`, `lsof`, and
`netstat` all answer the question directly, and the executor streams their
output straight to your terminal.

## Commands

| `kap` command | What it runs |
|---|---|
| `ports` | `ss -t -l -n -p`, or the `lsof` / `netstat` equivalent |

`tool = "auto"` (the default) prefers `ss` (part of iproute2, present on
essentially every modern Linux), falls back to `lsof` (the portable answer, and
the one that works on macOS), then `netstat`.

`kap ports -n` shows the exact command before running it, which is the fastest
way to see what kap decided.

## Configuration

Under `[plugins.ports]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tool` | `auto` \| `ss` \| `lsof` \| `netstat` | `auto` | Which tool to use |
| `listening_only` | bool | `true` | Only listening sockets, not established connections |
| `udp` | bool | `false` | Include UDP as well as TCP |
| `show_process` | bool | `true` | Ask for the owning process |

`show_process` usually needs root for sockets you do not own. Without it the
tools omit the process column rather than failing, so it is on by default.

```sh
kap ports --set udp=true --set listening_only=false
```

## Detection

Composable (§3.3) with priority 1, matching every directory: "what is listening
on this machine" is worth being able to ask from anywhere.

## Tests

```sh
kap plugin test ports
```

The four cases pin `project.tool()` through the case file's `tools` field, so
each of the three tools' argv forms is checked without depending on what is
installed on the machine running the tests.
