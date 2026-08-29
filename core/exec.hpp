#pragma once

// core/exec.hpp
//
// The executor (design doc §2 subsystem 4, §4 step 6, Milestone 5). It takes
// the `CommandSpec` a KPL command block produced and actually runs it.
//
// This file is the *only* place in kap that creates a process. §5.4 is
// explicit about why: "Plugins never spawn processes. A `step` appends to the
// spec; only the C++ executor calls posix_spawn. This is what makes
// `--dry-run`, logging, and sandboxing uniform." A plugin cannot run anything
// kap has not first rendered as an argv array it was willing to show you.
//
// ## No shell, ever — with one deliberate exception
//
// Steps are executed as argv arrays through `execvp`. No `system()`, no
// `/bin/sh -c`, no quoting round-trip: a directory named `my project` or a
// CMake define containing `;` reaches the tool exactly as the plugin wrote it,
// and a plugin cannot smuggle `; rm -rf ~` through a filename.
//
// The exception is hooks (§5.13), which are "plain shell strings run by the
// executor". Those come from the *user's own* kap.toml, not from a plugin, and
// a hook like `echo done && notify-send x` is only meaningful with a shell.
// The two paths are kept visibly separate below so the distinction cannot
// erode: run_steps() never sees a shell, run_hook() always does.
//
// ## Sequential vs. concurrent
//
// A sequential run lets children inherit kap's own stdout and stderr. That
// matters more than it sounds: an inherited terminal keeps `cargo build`'s
// colours, `ninja`'s progress line, and any tool's isatty() check working
// exactly as if you had typed the command yourself.
//
// A concurrent run (§5.4's `concurrent true`, used by `kap dev`) cannot do
// that — interleaved raw output from four dev servers is unreadable. Those
// steps get pipes, and each line is emitted with its step's label as a
// coloured prefix. Multiplexing is done with poll(2) in the main thread rather
// than one thread per child, so the process never forks while threads exist.

#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "core/kpl.hpp"

namespace kap
{
namespace exec
{

// How a finished process ended.
struct Outcome
{
    // The exit status kap should propagate. A child killed by signal N is
    // reported as 128 + N, the convention every POSIX shell uses, so
    // `kap build; echo $?` says the same thing `cmake --build .; echo $?`
    // would have.
    int exit_code = 0;

    // Set when the process could not be started at all (no such executable,
    // not executable, unreadable working directory). Distinguished from "the
    // tool ran and failed" because the advice is completely different.
    bool        spawn_failed = false;
    std::string spawn_error;

    bool ok() const
    {
        return exit_code == 0 && !spawn_failed;
    }
};

// Everything the executor needs that is not in the CommandSpec itself.
struct Options
{
    // The directory steps run in unless a step overrides it with its own
    // `cwd` (§5.4). Normally the detected project root.
    std::filesystem::path root;

    // --dry-run / -n: render what would run and execute nothing (§7: "always
    // shows the exact argv arrays before execution").
    bool dry_run = false;

    // --verbose: announce each step before running it.
    bool verbose = false;

    // Colourise labels and the dry-run rendering. Defaults to "only when
    // stdout is a terminal"; see `default_color()`.
    bool color = false;

    // Where kap's own narration goes. The child's output is not routed through
    // these — a sequential child writes straight to the inherited descriptors.
    std::ostream* out = nullptr; // defaults to std::cout
    std::ostream* err = nullptr; // defaults to std::cerr
};

// True when stdout is a terminal, which is when colour is wanted and when it
// is safe (a redirected build log should not collect escape sequences).
bool default_color();

// Run every step of `spec` (§4 step 6).
//
// Sequential specs stop at the first failing step: a build that failed has not
// earned its test run, and continuing would bury the real error. Concurrent
// specs let every step finish — `kap dev` killing three healthy dev servers
// because a fourth exited would be worse than useless — and report the first
// failure among them.
//
// Returns the outcome kap should exit with. Never throws for a failing child;
// a tool that exits 1 is an ordinary result, not an exceptional one.
Outcome run(const kpl::CommandSpec& spec, const Options& options);

// Run one hook string through `/bin/sh -c` (§5.13). `name` is the hook's key
// (e.g. "pre_build") and is used in messages. An empty command is a no-op that
// succeeds.
Outcome run_hook(const std::string& command, const std::string& name, const Options& options);

// Render one step the way --dry-run and --verbose show it: the argv array
// shell-quoted for display, plus any cwd/env/label the step carries. Exposed
// so tests can assert on the rendering without spawning anything.
std::string render_step(const kpl::Step& step, const Options& options);

// Total size in bytes of `path` — the file itself, or every regular file
// beneath it if it is a directory. Symlinks are counted as links, never
// followed, so a symlink into /usr cannot be reported as freed space.
// Unreadable entries contribute nothing rather than raising.
std::uintmax_t disk_usage(const std::filesystem::path& path);

// Render a byte count the way a human reads one ("1.4 MiB", "912 B").
std::string human_bytes(std::uintmax_t bytes);

} // namespace exec
} // namespace kap
