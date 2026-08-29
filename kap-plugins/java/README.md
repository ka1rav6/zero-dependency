# java

Maven and Gradle projects.

Java has two build tools and no lockfile to arbitrate between them, so this
plugin decides the way a person would: a `pom.xml` means Maven, a
`build.gradle` means Gradle, and a repository holding both is pinned in
`kap.toml` rather than guessed at.

## Detection

Claims a directory containing any of `pom.xml`, `build.gradle`,
`build.gradle.kts`, `settings.gradle`, or `settings.gradle.kts`. Priority 36.

That places it above `node` (35), so a repository with a `pom.xml` at the root
and a `package.json` for its frontend is treated as a Java project. If yours is
the other way round, say so:

```toml
[detect]
ecosystem = "node"
```

## The wrapper comes first

`./mvnw` and `./gradlew` exist because the version of the build tool is part of
the project, not part of the machine. Running a system `gradle` against a
project that ships `gradlew` is how you get an error that has nothing to do with
your code — so when a wrapper is present, it wins.

That is also why `requires` asks only for a JDK and lists `mvn` and `gradle` as
optional: a wrapper downloads the build tool itself, so a project can be
perfectly healthy with neither on `PATH`, and `kap doctor` should not claim
otherwise. Set `wrapper = false` to force the system tool.

## Commands

| `kap` command | Maven | Gradle |
|---|---|---|
| `build` | `package -DskipTests` | `assemble` |
| `check` | `compile` | `compileJava` |
| `test` | `test` | `test` |
| `run` | `exec:java -Dexec.mainClass=<main_class>` | `run` |
| `lint` | `<maven_lint_goal>` (default `checkstyle:check`) | `<gradle_lint_task>` (default `check`) |
| `fmt` | `<maven_fmt_goal>` (default `spotless:apply`) | `<gradle_fmt_task>` (default `spotlessApply`) |
| `install` | `install` | `publishToMavenLocal` |
| `clean` | `clean` | `clean`, and reports the space freed |

Each runs through `./mvnw` or `./gradlew` when the project ships one.

**`build` skips the tests.** Running them is Maven's default and almost never
what you want from a command called "build" — `kap test` is right there. Set
`skip_tests = false` to restore Maven's behaviour. Gradle's `assemble` is its
own "build without running the tests", so nothing has to be excluded by hand.

**`run` needs a main class under Maven.** Maven has no notion of "the" main
class, and `exec:java` without one fails deep inside the plugin with a message
about a missing parameter, so kap asks first:

```console
$ kap run
kap: error: set main_class to tell 'kap run' which class to execute: kap run --set main_class=com.example.Main, or [plugins.java] main_class = "com.example.Main" in kap.toml
```

Gradle projects using the `application` plugin need none of this — Gradle knows
its own `mainClass`.

**`fmt` and `lint` have no standard in Java.** The defaults follow Spotless and
Checkstyle because those are the most common choices, not because they are
universal. If your project uses something else, name the goal or task you
actually define; if it defines nothing, the build tool will say so.

## Configuration

Under `[plugins.java]`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tool` | `auto` \| `maven` \| `gradle` | `auto` | `auto` prefers Maven when a `pom.xml` is present |
| `wrapper` | bool | `true` | Use `./mvnw` or `./gradlew` when the project ships one |
| `main_class` | string | `""` | What `kap run` executes under Maven |
| `skip_tests` | bool | `true` | Whether `kap build` passes `-DskipTests` (Maven only) |
| `build_args` | list of strings | `[]` | Appended to every invocation, e.g. `["-o"]` for offline |
| `maven_fmt_goal` | string | `spotless:apply` | |
| `maven_fmt_check_goal` | string | `spotless:check` | Used when `check = true` |
| `maven_lint_goal` | string | `checkstyle:check` | |
| `gradle_fmt_task` | string | `spotlessApply` | |
| `gradle_fmt_check_task` | string | `spotlessCheck` | Used when `check = true` |
| `gradle_lint_task` | string | `check` | Gradle's own aggregate verification task |
| `check` | bool | `false` | `kap fmt --set check=true` verifies instead of rewriting |

```toml
[plugins.java]
tool       = "gradle"
main_class = "com.example.Main"
build_args = ["--offline"]
```

## Tests

```sh
kap plugin test java
```

Two fixtures: `maven-app` is a bare `pom.xml`, and `gradle-wrapper-app` ships a
`gradlew`, which is what covers the wrapper preference and the `wrapper = false`
override.
