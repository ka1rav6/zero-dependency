# node

npm, pnpm, yarn, and bun projects — with a workspace-aware `kap dev`.

## Detection

Claims any directory containing a `package.json`. Priority 35.

## Choosing the package manager

`package_manager = "auto"` (the default) reads the lockfile, which is the only
honest way to know: a repository with a `pnpm-lock.yaml` is a pnpm repository
whatever else happens to be installed on your machine.

| Lockfile present | Manager used |
|---|---|
| `pnpm-lock.yaml` | `pnpm` |
| `yarn.lock` | `yarn` |
| `bun.lockb` | `bun` |
| none of the above | `npm` |

Set `package_manager` explicitly to override.

## Commands

| `kap` command | What it runs |
|---|---|
| `build` | `<pm> run <build_script>` |
| `test` | `<pm> run <test_script>` |
| `check` | `<pm> run <check_script>` (default script: `typecheck`) |
| `lint` | `<pm> run <lint_script>` |
| `fmt` | `<pm> run <fmt_script>`, plus `-- --check` when `check = true` |
| `run` | `<pm> run <start_script>` |
| `install` | `<pm> install` |
| `dev` | see below |
| `clean` | `rm -rf` over `clean_paths`, and reports the space freed |

### `kap dev` in a workspace

If the root `package.json` mentions `"workspaces"` and `dev_all_workspaces` is
on (it is by default), `dev` emits one step per workspace and asks the executor
to run them **concurrently**, prefixing every line of output with the workspace
that produced it:

```
packages/api | listening on :3001
packages/web | vite ready in 240 ms
```

Ctrl-C stops all of them. `kap dev -n` shows exactly which commands would run
before any of them do.

Set `dev_all_workspaces = false` to run the root `dev` script instead.

## Configuration

Under `[plugins.node]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `package_manager` | `auto` \| `npm` \| `pnpm` \| `yarn` \| `bun` | `auto` | See above |
| `build_script` | string | `build` | |
| `test_script` | string | `test` | |
| `lint_script` | string | `lint` | |
| `fmt_script` | string | `format` | |
| `dev_script` | string | `dev` | |
| `start_script` | string | `start` | What `kap run` runs |
| `check_script` | string | `typecheck` | |
| `workspace_glob` | string | `packages/*` | Where `dev` looks for workspaces |
| `dev_all_workspaces` | bool | `true` | Run every workspace's `dev` at once |
| `check` | bool | `false` | Makes `fmt` verify instead of rewrite |
| `clean_paths` | list of strings | `["dist", "build", ".next", ".turbo", "coverage"]` | What `clean` removes |

`node_modules` is deliberately **not** in `clean_paths`: deleting it turns a
five-second clean into a five-minute reinstall. Add it yourself if that is what
you want.

```toml
[plugins.node]
package_manager = "pnpm"
workspace_glob = "apps/*"
clean_paths = ["dist", "node_modules"]
```

## Tests

```sh
kap plugin test node
```

The `workspace` fixture is a real two-package monorepo, so the concurrent `dev`
path is covered by a golden file rather than by hand-waving.
