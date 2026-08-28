# Developing kap with Docker

The whole project is designed to be built and tested inside a pinned Docker
image (design doc §10). This guide walks through every workflow: the first-time
setup, the daily edit/build/test loop, running CI, and how the pieces fit
together.

Prerequisites: Docker with the `docker compose` plugin (a.k.a. Compose v2).

- `docker --version`
- `docker compose version`

Everything else — cmake, ninja, g++, clang-format — lives in the image. You do
not need to install them on your host.

---

## 1. The three entry points, at a glance

| Command | What it is for |
|---|---|
| `docker compose run --rm dev` | The default onboarding path: an interactive shell in the dev container. |
| `docker compose run --rm dev ./scripts/ci.sh` | Headless CI: configure, build, unit-test, format-check, smoke-test. |
| `./scripts/in-docker.sh <cmd...>` | Sugar for the second form: run any command inside the container. |

All three use the exact same image (`docker/dev.Dockerfile`), which is why the
first build is slower than every later run — it pip-installs nothing, but it
does `apt-get` the toolchain and self-checks it with `kap-bootstrap
--check-deps`.

---

## 2. First time: clone, build, shell

```sh
git clone <this-repo> kap
cd kap

# Interactive dev shell (builds the image on the first run).
docker compose run --rm dev
```

Inside the container you are `root` at `/kap`, with the live repository
bind-mounted. **Files you edit on your host are immediately visible in the
container** — no rebuild needed, exactly like editing in a normal directory.

To verify everything works before diving in:

```sh
# one-shot self-check (also what CI runs)
./scripts/ci.sh
```

Or, interactively:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/kap --version        # expect: kap 0.1.0
```

> The `build/` directory is a named Docker volume (`kap-build`), so build
> products live in the container's storage, not in your checkout. `git status`
> stays clean and `git clean` stays safe.

---

## 3. The daily loop

Write code on the host, then either:

```sh
# (a) run one command inside the container
./scripts/in-docker.sh cmake --build build

# (b) drop into a shell for anything larger
docker compose run --rm dev
./scripts/ci.sh
```

`scripts/in-docker.sh` is just `docker compose run --rm dev "$@"` — it builds
the image if needed and discards the container when the command finishes.

Because the container is thrown away with `--rm`, **any state you want to keep
must live in a volume**:

| Volume | Mounted at | Purpose |
|---|---|---|
| `kap-build` | `/kap/build` | CMake/Ninja build products |
| `kap-cache` | `/root/.cache/kap` | kap's plugin/AST caches |
| `kap-config` | `/root/.config/kap` | kap's `config.toml`, global overrides |

The repo itself is a bind mount (not a volume), so it is *editable from both
sides*.

---

## 4. tl;dr command cheat sheet

| You want to… | Run |
|---|---|
| Get a shell in the dev environment | `docker compose run --rm dev` |
| Run the whole CI pipeline once | `docker compose run --rm dev ./scripts/ci.sh` |
| Build just the binary | `./scripts/in-docker.sh cmake --build build` |
| Run unit tests | `./scripts/in-docker.sh ctest --test-dir build --output-on-failure` |
| Rebuild the image (new apt packages) | `docker compose build dev` |
| Run the release-style container | `docker build -t kap . && docker run --rm kap --version` |
| Nuke all dev state | `docker compose down -v` (then rebuild) |

---

## 5. How it is pinned (and why that matters)

`docker/dev.Dockerfile` starts from an exact image digest:

```dockerfile
FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517
```

`ubuntu:24.04` is a mutable tag — a rebuild could silently pull a new
`24.04`. The digest cannot change, so **every clone and every CI run compiles
with the same glibc / gcc / cmake**. That is the whole point of the
"synchronized environment" (§10.5) and why the Dockerfile is the source of
truth, not your laptop.

The image build also self-checks the toolchain:

```dockerfile
COPY scripts/bootstrap.sh /usr/local/bin/kap-bootstrap
RUN kap-bootstrap --check-deps
```

If a dependency were missing or a `build-essential` package disappeared from
Ubuntu, the image build fails here instead of you discovering it at
compile time.

---

## 6. CI is the same container

`.github/workflows/ci.yml` does two steps:

```yaml
- run: docker compose build dev
- run: docker compose run --rm dev ./scripts/ci.sh
```

That is the identical image and the identical script you use locally — so "it
passes in CI but not on my machine" and its inverse are both impossible.

---

## 7. When you need an image different from dev

- **Release-style image** — repo-root `Dockerfile` builds the *binary* in a
  full toolchain then copies it into a minimal runtime image (multi-stage).
  Use it when you want `kap` as a deployable command.
- **`dev-full`** (with Rust, Node, Go, …) — planned for Milestone 10. It adds
  the ecosystem toolchains needed to run the full first-party plugin
  test-suite in one container.

---

## 8. Common problems

| Symptom | Likely cause / fix |
|---|---|
| `kap: /kap does not look like this repository` | You started a container without the bind mount (e.g. built and ran the dev image directly). Always go through compose or `in-docker.sh`. |
| First `docker compose run dev` is very slow | Expected — it is building the image. Subsequent runs are fast. |
| `permission denied` running `./scripts/ci.sh` | The scripts need the executable bit; `chmod +x scripts/*.sh docker/entrypoint.sh` if a re-checkout lost it. |
| `ctest` uses cached build output | The `kap-build` volume is persistent on purpose; `cmake --build build` and CTest handle the incremental part. For a truly clean build: `cmake --build build --target clean` (or `docker compose down -v`). |

---

## 9. Glossary of the moving parts

| File | Role |
|---|---|
| `docker/dev.Dockerfile` | The pinned dev image (toolchain + entrypoint). |
| `docker/entrypoint.sh` | Runs before each command; verifies `/kap` and exports `KAP_DEV=1`; `exec`s the user command. |
| `scripts/bootstrap.sh` | Self-contained setup; `--check-deps` at image build, full configure+build+test when run manually. |
| `scripts/ci.sh` | One-shot pipeline: configure → build → unit tests → format → smoke. |
| `scripts/in-docker.sh` | Wrapper that runs any command inside the dev container. |
| `docker-compose.yml` | Glues volumes, env, TTY, and `init` to the dev service. |