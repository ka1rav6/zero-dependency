// tests/test_exec.cpp
//
// Unit tests for the executor (core/exec.hpp, design doc Milestone 5).
//
// These tests really do spawn processes. That is deliberate: the whole point
// of this module is the fork/exec/wait dance, and a mocked version of it would
// only prove that the mock works. Every command used is a POSIX utility that
// is present anywhere kap builds at all (/bin/sh, true, false, echo, pwd, env,
// sleep), so no ecosystem toolchain is required — the same constraint the
// plugin fixture tests are held to.
//
// kap's own narration (dry-run rendering, labelled concurrent output, the
// freed-space line) is captured by pointing Options::out and Options::err at a
// std::ostringstream, so nothing here writes to the test runner's terminal.

#include "core/exec.hpp"
#include "core/kpl.hpp"
#include "harness.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{

std::filesystem::path scratch_root(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_exec_" + name + "_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

kap::kpl::Step step(std::vector<std::string> command)
{
    kap::kpl::Step s;
    s.command = std::move(command);
    return s;
}

kap::kpl::CommandSpec spec_of(std::vector<kap::kpl::Step> steps, bool concurrent = false)
{
    kap::kpl::CommandSpec spec;
    spec.steps      = std::move(steps);
    spec.concurrent = concurrent;
    return spec;
}

// Options wired to string streams, so a test can assert on what kap printed.
struct Captured
{
    std::ostringstream out;
    std::ostringstream err;
    kap::exec::Options options;

    explicit Captured(const std::filesystem::path& root = {})
    {
        options.root  = root;
        options.out   = &out;
        options.err   = &err;
        options.color = false; // deterministic output, no escape sequences
    }
};

} // namespace

// --- exit-code propagation ---------------------------------------------------------

KAP_TEST("a successful step yields exit code 0")
{
    Captured                 captured;
    const kap::exec::Outcome outcome = kap::exec::run(spec_of({step({"true"})}), captured.options);
    KAP_ASSERT(outcome.ok());
    KAP_ASSERT_EQ(outcome.exit_code, 0);
});

KAP_TEST("a failing step's exit code is propagated, never swallowed")
{
    // Design doc §4 step 7: "Exit with the underlying tool's exit code (never
    // swallow failures)."
    Captured                 captured;
    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({step({"/bin/sh", "-c", "exit 42"})}), captured.options);
    KAP_ASSERT(!outcome.ok());
    KAP_ASSERT_EQ(outcome.exit_code, 42);
});

KAP_TEST("a sequential run stops at the first failing step")
{
    const std::filesystem::path root = scratch_root("stop-on-fail");
    Captured                    captured(root);

    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({
                           step({"/bin/sh", "-c", "echo one > first.txt"}),
                           step({"/bin/sh", "-c", "exit 3"}),
                           step({"/bin/sh", "-c", "echo three > third.txt"}),
                       }),
                       captured.options);

    KAP_ASSERT_EQ(outcome.exit_code, 3);
    KAP_ASSERT(std::filesystem::exists(root / "first.txt"));
    // The step after the failure must not have run: a failed configure has not
    // earned its build, and running on would bury the error that matters.
    KAP_ASSERT(!std::filesystem::exists(root / "third.txt"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a child killed by a signal reports 128 + signal, as a shell would")
{
    Captured captured;
    // SIGTERM is 15, so the shell convention is 143.
    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({step({"/bin/sh", "-c", "kill -TERM $$"})}), captured.options);
    KAP_ASSERT_EQ(outcome.exit_code, 128 + 15);
});

KAP_TEST("a missing executable is reported as such, not as exit 127")
{
    // "cannot run 'kap-no-such-tool': No such file or directory" is actionable;
    // "exit status 127" is not. The distinction needs the exec-failure pipe.
    Captured                 captured;
    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({step({"kap-no-such-tool-xyz"})}), captured.options);
    KAP_ASSERT(outcome.spawn_failed);
    KAP_ASSERT_EQ(outcome.exit_code, 127);
    KAP_ASSERT(outcome.spawn_error.find("kap-no-such-tool-xyz") != std::string::npos);
    KAP_ASSERT(captured.err.str().find("cannot run") != std::string::npos);
});

KAP_TEST("an empty spec succeeds without spawning anything")
{
    Captured captured;
    KAP_ASSERT(kap::exec::run(spec_of({}), captured.options).ok());
});

// --- dry run -----------------------------------------------------------------------

KAP_TEST("--dry-run prints the argv arrays and runs nothing")
{
    const std::filesystem::path root = scratch_root("dry-run");
    Captured                    captured(root);
    captured.options.dry_run = true;

    const kap::exec::Outcome outcome = kap::exec::run(
        spec_of({step({"/bin/sh", "-c", "echo should-not-happen > proof.txt"})}), captured.options);

    KAP_ASSERT(outcome.ok());
    KAP_ASSERT(!std::filesystem::exists(root / "proof.txt"));
    KAP_ASSERT(captured.out.str().find("/bin/sh") != std::string::npos);
    KAP_ASSERT(captured.out.str().find("$ ") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("--dry-run quotes words that need it so the output is copy-pasteable")
{
    Captured captured;
    captured.options.dry_run = true;
    (void) kap::exec::run(spec_of({step({"cmake", "-B", "my build dir"})}), captured.options);
    KAP_ASSERT(captured.out.str().find("'my build dir'") != std::string::npos);
});

KAP_TEST("--dry-run announces concurrency and freed-space reporting")
{
    Captured captured;
    captured.options.dry_run   = true;
    kap::kpl::CommandSpec spec = spec_of({step({"a"}), step({"b"})}, true);
    spec.report_freed_space    = true;
    (void) kap::exec::run(spec, captured.options);
    KAP_ASSERT(captured.out.str().find("concurrently") != std::string::npos);
    KAP_ASSERT(captured.out.str().find("freed space") != std::string::npos);
});

// --- per-step cwd, env, label (§5.4) -------------------------------------------------

KAP_TEST("a step runs in the project root by default")
{
    const std::filesystem::path root = scratch_root("cwd-root");
    Captured                    captured(root);
    (void) kap::exec::run(spec_of({step({"/bin/sh", "-c", "pwd > where.txt"})}), captured.options);

    std::ifstream in(root / "where.txt");
    std::string   line;
    std::getline(in, line);
    KAP_ASSERT_EQ(std::filesystem::path(line), std::filesystem::weakly_canonical(root));

    std::filesystem::remove_all(root);
});

KAP_TEST("a step's own cwd is resolved relative to the root")
{
    const std::filesystem::path root = scratch_root("cwd-step");
    std::filesystem::create_directories(root / "packages" / "app");
    Captured captured(root);

    kap::kpl::Step s = step({"/bin/sh", "-c", "pwd > where.txt"});
    s.cwd            = "packages/app";
    (void) kap::exec::run(spec_of({s}), captured.options);

    KAP_ASSERT(std::filesystem::exists(root / "packages" / "app" / "where.txt"));
    KAP_ASSERT(!std::filesystem::exists(root / "where.txt"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a step's env entries reach the child")
{
    const std::filesystem::path root = scratch_root("env");
    Captured                    captured(root);

    kap::kpl::Step s = step({"/bin/sh", "-c", "printf '%s' \"$KAP_TEST_VAR\" > value.txt"});
    s.environment["KAP_TEST_VAR"] = "hello-from-kap";
    (void) kap::exec::run(spec_of({s}), captured.options);

    std::ifstream in(root / "value.txt");
    std::string   value;
    std::getline(in, value);
    KAP_ASSERT_EQ(value, std::string("hello-from-kap"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a step's env does not leak into the next step")
{
    // Each child gets its own copy of the environment; setting a variable for
    // one step must not silently configure the rest of the build.
    const std::filesystem::path root = scratch_root("env-isolated");
    Captured                    captured(root);

    kap::kpl::Step first              = step({"true"});
    first.environment["KAP_LEAK_VAR"] = "set";
    kap::kpl::Step second = step({"/bin/sh", "-c", "printf '[%s]' \"$KAP_LEAK_VAR\" > leak.txt"});

    (void) kap::exec::run(spec_of({first, second}), captured.options);

    std::ifstream in(root / "leak.txt");
    std::string   value;
    std::getline(in, value);
    KAP_ASSERT_EQ(value, std::string("[]"));

    std::filesystem::remove_all(root);
});

KAP_TEST("render_step shows the label, env, argv, and cwd")
{
    Captured       captured;
    kap::kpl::Step s      = step({"npm", "run", "dev"});
    s.cwd                 = "packages/app";
    s.label               = "app";
    s.environment["PORT"] = "3000";

    const std::string rendered = kap::exec::render_step(s, captured.options);
    KAP_ASSERT(rendered.find("[app]") != std::string::npos);
    KAP_ASSERT(rendered.find("PORT=3000") != std::string::npos);
    KAP_ASSERT(rendered.find("npm run dev") != std::string::npos);
    KAP_ASSERT(rendered.find("packages/app") != std::string::npos);
});

// --- concurrency (§5.11, `kap dev`) --------------------------------------------------

KAP_TEST("concurrent steps all run and their output is labelled")
{
    const std::filesystem::path root = scratch_root("concurrent");
    Captured                    captured(root);

    kap::kpl::Step first  = step({"/bin/sh", "-c", "echo hello-one"});
    first.label           = "one";
    kap::kpl::Step second = step({"/bin/sh", "-c", "echo hello-two"});
    second.label          = "two";

    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({first, second}, true), captured.options);

    KAP_ASSERT(outcome.ok());
    const std::string printed = captured.out.str();
    KAP_ASSERT(printed.find("one | hello-one") != std::string::npos);
    KAP_ASSERT(printed.find("two | hello-two") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("a concurrent step with no label falls back to its program name")
{
    Captured       captured;
    kap::kpl::Step first  = step({"/bin/echo", "alpha"});
    kap::kpl::Step second = step({"/bin/sh", "-c", "echo beta"});
    (void) kap::exec::run(spec_of({first, second}, true), captured.options);
    KAP_ASSERT(captured.out.str().find("/bin/echo | alpha") != std::string::npos);
});

KAP_TEST("concurrent output without a trailing newline is still flushed")
{
    // printf with no \n leaves a partial line held back waiting for one. If it
    // were never flushed, the last line of a tool's output would silently
    // vanish — the kind of bug that only shows up on the message that matters.
    Captured       captured;
    kap::kpl::Step only = step({"/bin/sh", "-c", "printf no-newline-here"});
    only.label          = "solo";
    (void) kap::exec::run(spec_of({only, step({"true"})}, true), captured.options);
    KAP_ASSERT(captured.out.str().find("solo | no-newline-here") != std::string::npos);
});

KAP_TEST("concurrent steps all run even when one fails")
{
    // `kap dev` killing three healthy dev servers because a fourth exited
    // would be worse than useless, so concurrent mode waits for everyone and
    // then reports the failure.
    const std::filesystem::path root = scratch_root("concurrent-fail");
    Captured                    captured(root);

    kap::kpl::Step failing = step({"/bin/sh", "-c", "exit 7"});
    failing.label          = "bad";
    kap::kpl::Step working = step({"/bin/sh", "-c", "echo ok > ran.txt"});
    working.label          = "good";

    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({failing, working}, true), captured.options);

    KAP_ASSERT_EQ(outcome.exit_code, 7);
    KAP_ASSERT(std::filesystem::exists(root / "ran.txt"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a concurrent step honours its own cwd")
{
    const std::filesystem::path root = scratch_root("concurrent-cwd");
    std::filesystem::create_directories(root / "a");
    std::filesystem::create_directories(root / "b");
    Captured captured(root);

    kap::kpl::Step first  = step({"/bin/sh", "-c", "touch here.txt"});
    first.cwd             = "a";
    first.label           = "a";
    kap::kpl::Step second = step({"/bin/sh", "-c", "touch here.txt"});
    second.cwd            = "b";
    second.label          = "b";

    (void) kap::exec::run(spec_of({first, second}, true), captured.options);
    KAP_ASSERT(std::filesystem::exists(root / "a" / "here.txt"));
    KAP_ASSERT(std::filesystem::exists(root / "b" / "here.txt"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a single concurrent step takes the sequential path unchanged")
{
    // `concurrent true` with one step has nothing to interleave, so it should
    // behave exactly like an ordinary run — including inheriting the terminal.
    const std::filesystem::path root = scratch_root("concurrent-one");
    Captured                    captured(root);
    const kap::exec::Outcome    outcome =
        kap::exec::run(spec_of({step({"/bin/sh", "-c", "touch ran.txt"})}, true), captured.options);
    KAP_ASSERT(outcome.ok());
    KAP_ASSERT(std::filesystem::exists(root / "ran.txt"));
    std::filesystem::remove_all(root);
});

// --- hooks (§5.13) -------------------------------------------------------------------

KAP_TEST("a hook runs through a shell, which is what makes it a hook")
{
    const std::filesystem::path root = scratch_root("hook");
    Captured                    captured(root);
    const kap::exec::Outcome    outcome =
        kap::exec::run_hook("echo one > a.txt && echo two > b.txt", "pre_build", captured.options);
    KAP_ASSERT(outcome.ok());
    KAP_ASSERT(std::filesystem::exists(root / "a.txt"));
    KAP_ASSERT(std::filesystem::exists(root / "b.txt"));
    std::filesystem::remove_all(root);
});

KAP_TEST("a failing hook reports its exit code and names itself")
{
    Captured                 captured;
    const kap::exec::Outcome outcome = kap::exec::run_hook("exit 9", "post_test", captured.options);
    KAP_ASSERT_EQ(outcome.exit_code, 9);
    KAP_ASSERT(captured.err.str().find("post_test") != std::string::npos);
});

KAP_TEST("an empty hook is a no-op that succeeds")
{
    Captured captured;
    KAP_ASSERT(kap::exec::run_hook("", "pre_build", captured.options).ok());
    KAP_ASSERT_EQ(captured.out.str(), std::string(""));
});

KAP_TEST("--dry-run prints a hook without running it")
{
    const std::filesystem::path root = scratch_root("hook-dry");
    Captured                    captured(root);
    captured.options.dry_run = true;
    (void) kap::exec::run_hook("touch should-not-exist.txt", "pre_build", captured.options);
    KAP_ASSERT(!std::filesystem::exists(root / "should-not-exist.txt"));
    KAP_ASSERT(captured.out.str().find("pre_build") != std::string::npos);
    std::filesystem::remove_all(root);
});

// --- freed space (§5.4's report_freed_space) -------------------------------------------

KAP_TEST("disk_usage sums a directory tree and ignores symlinks")
{
    const std::filesystem::path root = scratch_root("usage");
    write_file(root / "a.bin", std::string(1000, 'x'));
    write_file(root / "nested" / "b.bin", std::string(24, 'y'));
    KAP_ASSERT_EQ(kap::exec::disk_usage(root), static_cast<std::uintmax_t>(1024));

    // A symlink into a huge tree must not be reported as space kap freed.
    std::error_code ec;
    std::filesystem::create_symlink("/usr", root / "link", ec);
    if (!ec)
        KAP_ASSERT_EQ(kap::exec::disk_usage(root), static_cast<std::uintmax_t>(1024));

    KAP_ASSERT_EQ(kap::exec::disk_usage(root / "a.bin"), static_cast<std::uintmax_t>(1000));
    KAP_ASSERT_EQ(kap::exec::disk_usage(root / "nothing-here"), static_cast<std::uintmax_t>(0));

    std::filesystem::remove_all(root);
});

KAP_TEST("human_bytes renders sizes the way a person reads them")
{
    KAP_ASSERT_EQ(kap::exec::human_bytes(0), std::string("0 B"));
    KAP_ASSERT_EQ(kap::exec::human_bytes(912), std::string("912 B"));
    KAP_ASSERT_EQ(kap::exec::human_bytes(1024), std::string("1.0 KiB"));
    KAP_ASSERT_EQ(kap::exec::human_bytes(1536), std::string("1.5 KiB"));
    KAP_ASSERT_EQ(kap::exec::human_bytes(1024ull * 1024), std::string("1.0 MiB"));
    KAP_ASSERT_EQ(kap::exec::human_bytes(1024ull * 1024 * 1024 * 3), std::string("3.0 GiB"));
});

KAP_TEST("report_freed_space measures what a clean actually removed")
{
    const std::filesystem::path root = scratch_root("freed");
    write_file(root / "build" / "big.o", std::string(4096, 'z'));
    Captured captured(root);

    kap::kpl::CommandSpec spec = spec_of({step({"rm", "-rf", "build"})});
    spec.report_freed_space    = true;

    const kap::exec::Outcome outcome = kap::exec::run(spec, captured.options);
    KAP_ASSERT(outcome.ok());
    KAP_ASSERT(!std::filesystem::exists(root / "build"));
    KAP_ASSERT(captured.out.str().find("freed 4.0 KiB") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("report_freed_space reports zero rather than a negative number")
{
    // A "clean" that creates files must not report a nonsense negative size.
    const std::filesystem::path root = scratch_root("freed-grow");
    std::filesystem::create_directories(root / "build");
    Captured captured(root);

    kap::kpl::CommandSpec spec = spec_of({step({"/bin/sh", "-c", "echo grow > build/new.txt"})});
    spec.report_freed_space    = true;
    (void) kap::exec::run(spec, captured.options);

    KAP_ASSERT(captured.out.str().find("freed 0 B") != std::string::npos);

    std::filesystem::remove_all(root);
});

// --- find_url (Milestone 10: `kap dev -o`) --------------------------------------------
//
// The URL a dev server prints is the one line you always act on. Getting the
// boundaries right matters more than it looks: opening a URL with a sentence's
// full stop attached, or with a closing quote, produces a 404 rather than the
// app.

KAP_TEST("find_url picks a URL out of a line of tool output")
{
    KAP_ASSERT_EQ(kap::exec::find_url("  Local:   http://localhost:5173/"),
                  std::string("http://localhost:5173/"));
    KAP_ASSERT_EQ(kap::exec::find_url("Listening on https://127.0.0.1:8443/app?x=1&y=2"),
                  std::string("https://127.0.0.1:8443/app?x=1&y=2"));
});

KAP_TEST("find_url prefers https when a line offers both")
{
    // Tools that print both usually print the http one as a fallback note.
    KAP_ASSERT_EQ(kap::exec::find_url("http://a.example/ or https://b.example/"),
                  std::string("https://b.example/"));
});

KAP_TEST("find_url stops at whitespace and at wrapping punctuation")
{
    KAP_ASSERT_EQ(kap::exec::find_url("open http://localhost:3000 now"),
                  std::string("http://localhost:3000"));
    KAP_ASSERT_EQ(kap::exec::find_url("see \"http://localhost:3000\" for details"),
                  std::string("http://localhost:3000"));
    KAP_ASSERT_EQ(kap::exec::find_url("ready (http://localhost:3000)"),
                  std::string("http://localhost:3000"));
    KAP_ASSERT_EQ(kap::exec::find_url("[http://localhost:3000]"),
                  std::string("http://localhost:3000"));
});

KAP_TEST("find_url drops trailing sentence punctuation")
{
    // "serving at http://localhost:3000." is a sentence, not a path.
    KAP_ASSERT_EQ(kap::exec::find_url("serving at http://localhost:3000."),
                  std::string("http://localhost:3000"));
    KAP_ASSERT_EQ(kap::exec::find_url("try http://localhost:3000, or the other one"),
                  std::string("http://localhost:3000"));
});

KAP_TEST("find_url finds nothing when there is nothing to find")
{
    KAP_ASSERT_EQ(kap::exec::find_url(""), std::string(""));
    KAP_ASSERT_EQ(kap::exec::find_url("compiling 42 modules"), std::string(""));
    // A bare scheme is not a URL.
    KAP_ASSERT_EQ(kap::exec::find_url("the https:// scheme"), std::string(""));
});

KAP_TEST("find_url handles a URL at the very start and the very end of a line")
{
    KAP_ASSERT_EQ(kap::exec::find_url("http://a.example/x"), std::string("http://a.example/x"));
    KAP_ASSERT_EQ(kap::exec::find_url("go to http://a.example/x"),
                  std::string("http://a.example/x"));
});

KAP_TEST("open_first_url captures output and opens the URL a step printed")
{
    // -o forces the capturing path even for a single step, because kap cannot
    // read what it did not capture.
    //
    // Options::open_url is replaced so the test records the URL instead of
    // launching a browser. Without that seam this test really did open browser
    // windows on the machine running it — which is how the seam came to exist.
    Captured                 captured;
    std::vector<std::string> opened;
    captured.options.open_first_url = true;
    captured.options.open_url       = [&opened](const std::string& url) {
        opened.push_back(url);
        return true;
    };

    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({step({"/bin/sh", "-c", "echo Local: http://127.0.0.1:60123/"})}),
                       captured.options);

    KAP_ASSERT(outcome.ok());
    KAP_ASSERT_EQ(opened.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(opened.front(), std::string("http://127.0.0.1:60123/"));
    KAP_ASSERT(captured.out.str().find("kap: opened") != std::string::npos);
});

KAP_TEST("open_first_url opens at most one URL, however many are printed")
{
    Captured                 captured;
    std::vector<std::string> opened;
    captured.options.open_first_url = true;
    captured.options.open_url       = [&opened](const std::string& url) {
        opened.push_back(url);
        return true;
    };

    (void) kap::exec::run(
        spec_of({step({"/bin/sh",
                       "-c",
                       "echo http://127.0.0.1:60001/; sleep 0.05; echo http://127.0.0.1:60002/"})}),
        captured.options);

    KAP_ASSERT_EQ(opened.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(opened.front().find("60001") != std::string::npos);
});

KAP_TEST("open_first_url reports honestly when no opener is available")
{
    // "found it but could not open it" is a different fact from "opened it",
    // and saying the wrong one sends someone looking at their browser.
    Captured captured;
    captured.options.open_first_url = true;
    captured.options.open_url       = [](const std::string&) { return false; };

    (void) kap::exec::run(spec_of({step({"/bin/sh", "-c", "echo http://127.0.0.1:60123/"})}),
                          captured.options);

    KAP_ASSERT(captured.out.str().find("kap: found") != std::string::npos);
    KAP_ASSERT(captured.out.str().find("kap: opened") == std::string::npos);
});

KAP_TEST("open_first_url without a URL opens nothing")
{
    Captured                 captured;
    std::vector<std::string> opened;
    captured.options.open_first_url = true;
    captured.options.open_url       = [&opened](const std::string& url) {
        opened.push_back(url);
        return true;
    };

    const kap::exec::Outcome outcome =
        kap::exec::run(spec_of({step({"/bin/sh", "-c", "echo just building"})}), captured.options);

    KAP_ASSERT(outcome.ok());
    KAP_ASSERT(opened.empty());
    KAP_ASSERT(captured.out.str().find("just building") != std::string::npos);
});
