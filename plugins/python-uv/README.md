# python-uv

Python projects managed with [uv](https://docs.astral.sh/uv/).

Every command is `uv run <tool>`, so no step ever has to activate a virtual
environment or worry about which interpreter is on PATH — `uv run` settles both
questions itself.

## Detection

Claims a directory containing `uv.lock` **or** `pyproject.toml`. Priority 38.

A `uv.lock` is unambiguous. A bare `pyproject.toml` is not — it might be a
poetry, hatch, or pdm project — but uv works with any standard pyproject, so
claiming it is more useful than leaving the directory unowned. If you use a
different tool, install its plugin (a higher priority wins) or pin yours:

```toml
# kap.toml
[detect]
ecosystem = "python-poetry"
```

## Commands

| `kap` command | What it runs |
|---|---|
| `build` | `uv build` — sdist and wheel |
| `test` | `uv run pytest`, or `uv run python -m unittest discover` |
| `lint` | `uv run ruff check <paths>` (or flake8) |
| `fmt` | `uv run ruff format <paths>`, plus `--check` when `check = true` (or black) |
| `check` | `uv run mypy`/`pyright`, or `uv run ruff check` when no type checker is configured |
| `install` | `uv sync` — the "get me set up to work on this" command |
| `run` | `uv run python -m <module>`, or plain `uv run` plus whatever follows `--` |
| `dev` | `uv run python -m <module>` |
| `clean` | `rm -rf` over `clean_paths`, and reports the space freed |
| `ci` | `ruff format --check`, then `ruff check`, then `pytest` |

With no `module` configured, `kap run` passes everything after `--` straight to
`uv run`, so this works with no configuration at all:

```sh
kap run -- ./scripts/migrate.py
```

## Configuration

Under `[plugins.python-uv]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `test_runner` | `pytest` \| `unittest` | `pytest` | |
| `linter` | `ruff` \| `flake8` | `ruff` | |
| `formatter` | `ruff` \| `black` | `ruff` | |
| `type_checker` | `off` \| `mypy` \| `pyright` | `off` | `off` falls back to `ruff check`, which is a real static pass and already installed |
| `module` | string | `""` | What `run` and `dev` execute via `python -m` |
| `paths` | list of strings | `["."]` | What lint, fmt, and check look at |
| `check` | bool | `false` | Makes `fmt` verify instead of rewrite |
| `clean_paths` | list of strings | `["build", "dist", ".pytest_cache", ".ruff_cache", ".mypy_cache"]` | What `clean` removes |

`.venv` is deliberately **not** in `clean_paths`: deleting it turns a clean into
a full reinstall.

The `type_checker` member is called `off`, not `none`, because `none` is KPL's
absent-value literal and cannot appear as a match pattern. kap refuses a schema
that declares an enum member named `none` and tells you so, rather than letting
you find out from a confusing exhaustiveness error later.

```toml
[plugins.python-uv]
type_checker = "mypy"
module = "myapp"
paths = ["src", "tests"]
```

## Tests

```sh
kap plugin test python-uv
```
