# AGENTS.md

Guidelines for AI agents (and human contributors) working in this repository.
Read this file before making any changes and follow every rule below.

---

## 1. Project overview

- **Name:** `kap` ("know project, act"), in the `zero-dependency` repository.
- **Goal:** Hackathon submission demonstrating software built with **zero external
  dependencies** — std library + POSIX APIs only.
- **Language:** C++20 (`CMAKE_CXX_STANDARD 20`, no compiler extensions).

---

## 2. Non-negotiable rules

These rules apply without fail, in every change, no exceptions:

- **Zero dependencies.** No external libraries, no third-party packages, no
  vendored code. Only the C++ standard library and POSIX APIs are allowed.
  Before adding any `#include`, ask whether it can be satisfied by std/POSIX.
- **Everything must work.** Every change must keep the project building and
  tests passing.
- **Maximize performance** while following the design doc.

---

## 3. Design doc & roadmap

- Always follow the **design doc** and the roadmap contained in it
  (`docs/design.md`).
- **Ask before making a new design choice** that is not already covered by the
  design doc. Do not silently deviate.

---

## 4. Commit workflow

- **Commit a lot.** Every single logical change and feature gets its own commit.
- Write **verbose** commit messages that explain *what* and *why*.
- Do not bundle unrelated changes into one commit.
- Follow the repo's existing commit message style.

---

## 5. Code quality

- **Comment a lot.** The codebase should be understandable by beginners.
  Explain *why* a piece of code exists, not just *what* it does.
- Keep the code readable and idiomatic C++.
- Match the existing code conventions of the surrounding files.

---

## 6. Testing

- **Create unit tests for everything.**
- Test **every feature separately** so failures are isolated and diagnosable.
- Use only std/POSIX tooling for tests — no external test frameworks.
- Always run the full test suite before finishing any work.

---

## 7. CI pipeline

- Maintain a proper **CI pipeline** that verifies the build is appropriate.
- The pipeline must build and run the full test suite on every change.
- Keep CI green at all times.

---

## 8. Development environment

- Provide an easily reproducible dev environment (via **Docker** only).
- The container must be usable for building, testing, and running the project.
- Document the recommended local build/test commands here (see below).

### Build & test commands

`scripts/ci.sh` is the single entry point: it configures, builds with
`-DKAP_WERROR=ON`, runs the unit tests and the e2e suite, enforces
`clang-format`, exercises every plugin, checks an install into a scratch
prefix, and builds the `-DKAP_EMBED_PLUGINS=ON` variant. It is exactly what CI
runs, so a green run here means a green run there.

```sh
# Everything CI does, in the pinned container (the recommended path)
docker compose run --rm dev ./scripts/ci.sh

# ...or on the host, if you have cmake, ninja, and clang-format
./scripts/ci.sh
```

For a faster inner loop:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKAP_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Run `./scripts/ci.sh` before calling any change finished.
