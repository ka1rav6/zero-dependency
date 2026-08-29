# go

Build, test, vet, and format Go modules.

Go's toolchain is unusually uniform — one binary, one set of subcommands, no
package-manager choice to make — which makes this the shortest ecosystem plugin
kap ships and a good one to read first if you are writing your own.

## Detection

Claims a directory containing `go.mod` or `go.work`. Priority 45.

## Commands

| `kap` command | What it runs |
|---|---|
| `build` | `go build [-tags ...] [-o ...] <build_flags> <packages>` |
| `test` | `go test [-race] [-tags ...] <packages>` |
| `check` | `go vet <packages>` — the closest thing Go has to a typecheck-only pass |
| `lint` | `golangci-lint run` when it is installed, else `go vet` |
| `fmt` | `gofmt -l -w .`, or `gofmt -l .` when `check = true` |
| `run` | `go run <packages>` |
| `install` | `go install <packages>` |
| `clean` | `go clean -cache -testcache <packages>`, and reports the space freed |
| `ci` | `gofmt -l .`, then `go vet`, then `go test [-race]` |

One honest caveat about `fmt --set check=true`: `gofmt` exits 0 even when `-l`
printed filenames, so a CI job that only inspects the exit code will not notice
misformatted files. That is `gofmt`'s behaviour, not kap's. The usual answer is
to fail when the output is non-empty:

```sh
test -z "$(kap fmt --set check=true)"
```

## Configuration

Under `[plugins.go]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `packages` | string | `./...` | What to build, test, and vet |
| `output` | string | `""` | `-o` for `build`; empty lets go decide |
| `tags` | string | `""` | `-tags a,b` |
| `race` | bool | `true` | `-race` on `test` and `ci`. On by default because race bugs are the hardest kind to find any other way; turn it off for a build without cgo |
| `build_flags` | list of strings | `[]` | Extra flags for `build` |
| `linter` | `auto` \| `vet` \| `golangci` | `auto` | `auto` prefers `golangci-lint` when it is on PATH |
| `check` | bool | `false` | Makes `fmt` report instead of rewrite |

```toml
[plugins.go]
packages = "./cmd/..."
output = "bin/server"
tags = "netgo,osusergo"
build_flags = ["-trimpath"]
```

## Tests

```sh
kap plugin test go
```

Two of the cases declare `"tools": ["golangci-lint"]` and `"tools": []`, which
is how a fixture pins what `project.tool()` reports — the plugin's linter choice
is then tested without depending on what is installed on the machine running
the tests.
