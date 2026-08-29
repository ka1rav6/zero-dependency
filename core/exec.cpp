// core/exec.cpp
//
// Implementation of the executor declared in core/exec.hpp (design doc
// Milestone 5).

#include "core/exec.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/argv.hpp"
#include "core/diag.hpp"

namespace kap
{
namespace exec
{

namespace
{

// --- signal forwarding (Milestone 5: "SIGINT forwards to all children") ---------
//
// When kap runs in a terminal, Ctrl-C already reaches the whole foreground
// process group, children included. It is the *other* cases that need this: a
// SIGTERM from a supervisor, a `kill` aimed at kap's pid, or kap being run
// from a script where it is not the foreground group leader. Without
// forwarding, kap would exit and leave a detached `cargo build` running.
//
// Everything here has to be async-signal-safe, which rules out std::vector,
// locks, and iostreams. A fixed-size array of volatile sig_atomic_t written by
// the main thread and read by the handler is the standard shape, and kill(2)
// is on POSIX's async-signal-safe list.

constexpr std::size_t kMaxTrackedChildren = 64;

volatile sig_atomic_t g_children[kMaxTrackedChildren];
volatile sig_atomic_t g_child_count = 0;

// Set by the handler so the main loop can tell "the tool failed" from "the
// user interrupted us" and report the second one honestly.
volatile sig_atomic_t g_interrupted = 0;

extern "C" void forward_signal(int signal_number)
{
    g_interrupted            = signal_number;
    const sig_atomic_t count = g_child_count;
    for (sig_atomic_t index = 0;
         index < count && index < static_cast<sig_atomic_t>(kMaxTrackedChildren);
         ++index) {
        const pid_t pid = static_cast<pid_t>(g_children[index]);
        if (pid > 0)
            ::kill(pid, signal_number);
    }
}

void track_child(pid_t pid)
{
    const sig_atomic_t count = g_child_count;
    if (count < static_cast<sig_atomic_t>(kMaxTrackedChildren)) {
        g_children[count] = static_cast<sig_atomic_t>(pid);
        // Publish the pid *before* the count, so a signal arriving between the
        // two writes sees a shorter list rather than an uninitialised slot.
        g_child_count = count + 1;
    }
}

void untrack_children()
{
    g_child_count = 0;
}

// Install the forwarding handlers for the duration of one run, restoring
// whatever was there before. A RAII type because every early return in the
// run loops would otherwise have to remember to restore them.
class SignalGuard
{
public:
    SignalGuard()
    {
        g_interrupted = 0;
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_handler = forward_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0; // no SA_RESTART: a blocking poll() should return EINTR
        ::sigaction(SIGINT, &action, &previous_int_);
        ::sigaction(SIGTERM, &action, &previous_term_);

        // A child writing to a pipe kap has already closed must not kill kap.
        // Ignoring SIGPIPE turns that into a plain EPIPE on write().
        struct sigaction ignore;
        std::memset(&ignore, 0, sizeof(ignore));
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ignore.sa_flags = 0;
        ::sigaction(SIGPIPE, &ignore, &previous_pipe_);
    }

    ~SignalGuard()
    {
        ::sigaction(SIGINT, &previous_int_, nullptr);
        ::sigaction(SIGTERM, &previous_term_, nullptr);
        ::sigaction(SIGPIPE, &previous_pipe_, nullptr);
        untrack_children();
    }

    SignalGuard(const SignalGuard&)            = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;

private:
    struct sigaction previous_int_
    {};

    struct sigaction previous_term_
    {};

    struct sigaction previous_pipe_
    {};
};

// --- colour --------------------------------------------------------------------

// Six readable foreground colours, cycled per concurrent step so four dev
// servers are told apart at a glance. Deliberately no red — red is reserved
// for kap's own error output, and a dev server whose label happened to be red
// would look like a failure.
constexpr std::array<const char*, 6> kLabelColors = {
    "\033[36m", // cyan
    "\033[32m", // green
    "\033[33m", // yellow
    "\033[35m", // magenta
    "\033[34m", // blue
    "\033[96m", // bright cyan
};
constexpr const char* kReset = "\033[0m";
constexpr const char* kDim   = "\033[2m";

std::ostream& out_stream(const Options& options)
{
    return options.out != nullptr ? *options.out : std::cout;
}

std::ostream& err_stream(const Options& options)
{
    return options.err != nullptr ? *options.err : std::cerr;
}

// --- process launching -----------------------------------------------------------

// One running child and, when its output is being captured, the read end of
// its pipe.
struct Child
{
    pid_t       pid       = -1;
    int         output_fd = -1; // -1 when the child inherited kap's streams
    std::string label;
    std::string color;
    std::string pending; // a partial last line, waiting for its '\n'
    bool        spawn_failed = false;
    std::string spawn_error;
    int         exit_code = 0;
    bool        reaped    = false;
};

// Convert a wait(2) status into the code a shell would report.
int status_to_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status); // the universal shell convention
    return 1;
}

// Start one process.
//
// fork + execvp rather than posix_spawn, for one concrete reason: a step may
// carry its own `cwd` (§5.4), and changing a spawned process's directory needs
// posix_spawn_file_actions_addchdir_np — a glibc extension that musl and older
// glibc do not have. In the child, between fork and exec, chdir(2) is one
// portable line. §9 explicitly permits "posix_spawn / fork+execve".
//
// `capture` asks for a pipe; otherwise the child inherits kap's descriptors so
// an interactive tool keeps its terminal.
Child spawn_process(const std::vector<std::string>&           command,
                    const std::optional<std::string>&         cwd,
                    const std::map<std::string, std::string>& environment,
                    const std::filesystem::path&              root,
                    bool                                      capture)
{
    Child child;

    if (command.empty()) {
        child.spawn_failed = true;
        child.spawn_error  = "empty command";
        return child;
    }

    // The child reports an exec failure through this pipe. Without it, a
    // missing tool is indistinguishable from a tool that genuinely exited 127,
    // and "cmake: command not found" is far more useful than "exit status 127".
    // O_CLOEXEC is what makes the read end see EOF the moment exec succeeds.
    int error_pipe[2] = {-1, -1};
    if (::pipe(error_pipe) != 0) {
        child.spawn_failed = true;
        child.spawn_error  = std::string("cannot create a pipe: ") + std::strerror(errno);
        return child;
    }
    ::fcntl(error_pipe[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC);

    int output_pipe[2] = {-1, -1};
    if (capture && ::pipe(output_pipe) != 0) {
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        child.spawn_failed = true;
        child.spawn_error  = std::string("cannot create a pipe: ") + std::strerror(errno);
        return child;
    }

    // Build the argv array before forking. Everything the child needs must
    // already exist: after fork() only async-signal-safe calls are strictly
    // legal, and allocating there is the classic way to deadlock.
    std::vector<char*> raw;
    raw.reserve(command.size() + 1);
    for (const std::string& word : command)
        raw.push_back(const_cast<char*>(word.c_str()));
    raw.push_back(nullptr);

    const std::filesystem::path working_directory      = cwd.has_value() ? (root / *cwd) : root;
    const std::string           working_directory_text = working_directory.string();

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        if (capture) {
            ::close(output_pipe[0]);
            ::close(output_pipe[1]);
        }
        child.spawn_failed = true;
        child.spawn_error  = std::string("cannot fork: ") + std::strerror(errno);
        return child;
    }

    if (pid == 0) {
        // --- child ---------------------------------------------------------
        // Anything that fails here writes its errno to the error pipe and
        // exits 127; the parent turns that into a real message.
        const auto die = [&](int code) {
            const int saved   = errno;
            ssize_t   ignored = ::write(error_pipe[1], &saved, sizeof(saved));
            (void) ignored;
            ::_exit(code);
        };

        ::close(error_pipe[0]);

        if (capture) {
            ::close(output_pipe[0]);
            // Both streams go down one pipe, so the interleaving the tool
            // intended between its own stdout and stderr is preserved.
            if (::dup2(output_pipe[1], STDOUT_FILENO) < 0)
                die(127);
            if (::dup2(output_pipe[1], STDERR_FILENO) < 0)
                die(127);
            ::close(output_pipe[1]);
        }

        if (!working_directory_text.empty() && ::chdir(working_directory_text.c_str()) != 0)
            die(127);

        // Per-step environment (§5.4). setenv() is not on the async-signal-safe
        // list, but the restriction that motivates that list is about forking
        // from a *multi-threaded* process; kap never spawns while threads are
        // running (the concurrent path multiplexes with poll, not threads), so
        // no lock can be held here by a thread that no longer exists.
        for (const auto& [name, value] : environment)
            ::setenv(name.c_str(), value.c_str(), 1);

        ::execvp(raw[0], raw.data());
        die(127); // execvp only returns on failure
    }

    // --- parent ------------------------------------------------------------
    ::close(error_pipe[1]);
    if (capture)
        ::close(output_pipe[1]);

    // A successful exec closes the write end without writing, so this read
    // returns 0. Any other result is the child's errno.
    int     child_errno = 0;
    ssize_t got         = ::read(error_pipe[0], &child_errno, sizeof(child_errno));
    ::close(error_pipe[0]);

    if (got == static_cast<ssize_t>(sizeof(child_errno))) {
        int status = 0;
        ::waitpid(pid, &status, 0); // reap the 127 so it cannot become a zombie
        if (capture)
            ::close(output_pipe[0]);
        child.spawn_failed = true;
        child.spawn_error  = "cannot run '" + command.front() + "': " + std::strerror(child_errno);
        return child;
    }

    child.pid       = pid;
    child.output_fd = capture ? output_pipe[0] : -1;
    return child;
}

// --- concurrent output ------------------------------------------------------------

// Emit `text` from one child, splitting it into whole lines and prefixing each
// with the child's label. A partial trailing line is held in `pending` until
// its newline arrives, so a prefix never lands mid-line.
void emit_labelled(Child& child, std::string_view text, const Options& options)
{
    std::ostream& out = out_stream(options);
    child.pending.append(text);

    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = child.pending.find('\n', start);
        if (newline == std::string::npos)
            break;
        const std::string_view line(child.pending.data() + start, newline - start);
        if (options.color)
            out << child.color << child.label << " |" << kReset << ' ' << line << '\n';
        else
            out << child.label << " | " << line << '\n';
        start = newline + 1;
    }
    child.pending.erase(0, start);
    out.flush();
}

// Flush whatever a child left without a trailing newline.
void flush_pending(Child& child, const Options& options)
{
    if (child.pending.empty())
        return;
    child.pending.push_back('\n');
    emit_labelled(child, {}, options);
}

// --- freed-space accounting (§5.4's report_freed_space) -----------------------------

// Which paths a spec is about to affect, for the before/after measurement.
//
// The honest options were "measure the whole project root" (correct but a full
// walk of a possibly huge tree) and "measure nothing and print a guess". This
// takes the middle road: any argv word that names something that exists under
// the root right now is a path the command is plausibly about to remove, so
// measure those. `step rm "-rf" "build"` measures build/. A command with no
// path-shaped argument (`cargo clean`) falls back to the whole root, which is
// slower but is the only way to see what it removed.
std::vector<std::filesystem::path> freed_space_candidates(const kpl::CommandSpec&      spec,
                                                          const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> candidates;
    for (const kpl::Step& step : spec.steps) {
        for (std::size_t index = 1; index < step.command.size(); ++index) {
            const std::string& word = step.command[index];
            if (word.empty() || word.front() == '-')
                continue;
            std::error_code             ec;
            const std::filesystem::path candidate = root / word;
            if (!std::filesystem::exists(candidate, ec) || ec)
                continue;
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                candidates.push_back(candidate);
        }
    }
    if (candidates.empty())
        candidates.push_back(root);
    return candidates;
}

std::uintmax_t total_usage(const std::vector<std::filesystem::path>& paths)
{
    std::uintmax_t total = 0;
    for (const std::filesystem::path& path : paths)
        total += disk_usage(path);
    return total;
}

} // namespace

std::string find_url(std::string_view text)
{
    for (const std::string_view scheme :
         {std::string_view("https://"), std::string_view("http://")}) {
        const std::size_t start = text.find(scheme);
        if (start == std::string_view::npos)
            continue;

        std::size_t end = start;
        while (end < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[end]);
            // Stop at whitespace, at a control character, and at the quotes and
            // brackets tools wrap URLs in. Everything else is fair game — a URL
            // may legitimately contain '?', '&', '#', and '='.
            if (ch <= ' ' || ch == 0x7F || ch == '"' || ch == '\'' || ch == '<' || ch == '>' ||
                ch == '`' || ch == ')' || ch == ']')
                break;
            ++end;
        }

        // Trailing sentence punctuation is almost never part of the URL:
        // "serving at http://localhost:3000." is a sentence, not a path.
        while (end > start && (text[end - 1] == '.' || text[end - 1] == ',' ||
                               text[end - 1] == ';' || text[end - 1] == ':'))
            --end;

        // "https://" alone is a scheme, not a URL.
        if (end - start > scheme.size())
            return std::string(text.substr(start, end - start));
    }
    return {};
}

bool open_url(const std::string& url)
{
    // Two candidates, tried in order: xdg-open is the freedesktop standard,
    // `open` is macOS. Neither being present is a normal outcome on a headless
    // machine, not an error worth failing the dev loop over.
    for (const char* opener : {"xdg-open", "open"}) {
        Child child =
            spawn_process({opener, url}, std::nullopt, {}, std::filesystem::current_path(), false);
        if (child.spawn_failed)
            continue;

        // Deliberately not waited on. A browser can take seconds to start, and
        // blocking the dev loop on it would be worse than the zombie this
        // leaves — which the process reaps at exit anyway, since kap does not
        // outlive the dev session.
        return true;
    }
    return false;
}

bool default_color()
{
    // Two gates, both necessary. isatty answers "would anyone see colour" — a
    // redirected build log should not collect escape sequences. NO_COLOR
    // (no-color.org, honoured by ripgrep, bat, cargo, and friends) answers
    // "does this user want it", and its convention is that *any* value counts,
    // including the empty string, so presence alone is the test.
    if (std::getenv("NO_COLOR") != nullptr)
        return false;
    return ::isatty(STDOUT_FILENO) == 1;
}

std::uintmax_t disk_usage(const std::filesystem::path& path)
{
    std::error_code ec;

    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec)
        return 0;
    if (std::filesystem::is_symlink(status))
        return 0; // never follow: a link into /usr is not freed space
    if (std::filesystem::is_regular_file(status)) {
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        return ec ? 0 : size;
    }
    if (!std::filesystem::is_directory(status))
        return 0;

    std::uintmax_t total = 0;
    // skip_permission_denied keeps an unreadable subdirectory from turning a
    // size report into a thrown exception.
    std::filesystem::recursive_directory_iterator it(
        path, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_symlink(entry_ec) || entry_ec)
            continue;
        if (!it->is_regular_file(entry_ec) || entry_ec)
            continue;
        const std::uintmax_t size = it->file_size(entry_ec);
        if (!entry_ec)
            total += size;
    }
    return total;
}

std::string human_bytes(std::uintmax_t bytes)
{
    constexpr std::array<const char*, 5> kUnits = {"B", "KiB", "MiB", "GiB", "TiB"};

    if (bytes < 1024)
        return std::to_string(bytes) + " B";

    // Keep one decimal place without touching floating point, which would drag
    // in locale-dependent formatting for a number that is only ever displayed:
    // scale by 10, divide, then place the point by hand.
    std::uintmax_t scaled = bytes * 10;
    std::size_t    unit   = 0;
    while (scaled >= 10ull * 1024 && unit + 1 < kUnits.size()) {
        scaled /= 1024;
        ++unit;
    }
    const std::uintmax_t whole    = scaled / 10;
    const std::uintmax_t fraction = scaled % 10;
    return std::to_string(whole) + "." + std::to_string(fraction) + " " + kUnits[unit];
}

std::string render_step(const kpl::Step& step, const Options& options)
{
    std::string text;
    if (step.label.has_value())
        text += "[" + *step.label + "] ";
    for (const auto& [name, value] : step.environment)
        text += name + "=" + argv::escape_word(value) + " ";
    text += argv::List(step.command).quoted();
    if (step.cwd.has_value())
        text += "   (in " + *step.cwd + ")";
    (void) options;
    return text;
}

Outcome run(const kpl::CommandSpec& spec, const Options& options)
{
    Outcome outcome;

    // A `fail` statement means the plugin looked at this project and concluded
    // there is nothing runnable to do. It is checked before everything else —
    // including the dry-run path and the empty-steps shortcut — because the
    // message is the whole result, and because any steps accumulated before
    // the `fail` were never going to run.
    if (spec.failure.has_value()) {
        std::ostream& err = err_stream(options);
        err << "kap: error: " << *spec.failure << "\n";
        if (options.dry_run && !spec.steps.empty()) {
            err << "      note: the plan stopped here; it would have run:\n";
            for (const kpl::Step& step : spec.steps)
                err << "      note:   " << render_step(step, options) << "\n";
        }
        err.flush();
        outcome.exit_code = 1;
        return outcome;
    }

    if (spec.steps.empty())
        return outcome;

    // --dry-run: render and stop (§7). Nothing below this point runs.
    if (options.dry_run) {
        std::ostream& out = out_stream(options);
        for (const kpl::Step& step : spec.steps) {
            if (options.color)
                out << kDim << "$ " << kReset;
            else
                out << "$ ";
            out << render_step(step, options) << '\n';
        }
        if (spec.concurrent && spec.steps.size() > 1)
            out << (options.color ? kDim : "") << "# the steps above run concurrently"
                << (options.color ? kReset : "") << '\n';
        if (spec.report_freed_space)
            out << (options.color ? kDim : "") << "# freed space would be reported"
                << (options.color ? kReset : "") << '\n';
        out.flush();
        return outcome;
    }

    const std::filesystem::path root =
        options.root.empty() ? std::filesystem::current_path() : options.root;

    // Measure before, so `kap clean` can say what it recovered (§5.4).
    std::vector<std::filesystem::path> measured;
    std::uintmax_t                     before = 0;
    if (spec.report_freed_space) {
        measured = freed_space_candidates(spec, root);
        before   = total_usage(measured);
    }

    SignalGuard guard;

    // -o has to read the output to find a URL in it, so it takes the capturing
    // path even for a single step that would otherwise inherit the terminal.
    if ((spec.concurrent && spec.steps.size() > 1) ||
        (options.open_first_url && !spec.steps.empty())) {
        // --- concurrent (§3.3, §5.11: `kap dev`) -----------------------------
        std::vector<Child> children;
        children.reserve(spec.steps.size());

        for (std::size_t index = 0; index < spec.steps.size(); ++index) {
            const kpl::Step& step = spec.steps[index];
            Child child = spawn_process(step.command, step.cwd, step.environment, root, true);
            child.label = step.label.value_or(step.command.empty() ? std::string("step")
                                                                   : step.command.front());
            child.color = kLabelColors[index % kLabelColors.size()];
            if (!child.spawn_failed)
                track_child(child.pid);
            children.push_back(std::move(child));
        }

        bool opened_a_url = false;

        // Multiplex every child's pipe with poll(2). One loop in one thread:
        // no thread ever exists while a fork is in flight, which is what keeps
        // the setenv() in the child above safe.
        for (;;) {
            std::vector<pollfd>      fds;
            std::vector<std::size_t> owners;
            for (std::size_t index = 0; index < children.size(); ++index) {
                if (children[index].output_fd < 0)
                    continue;
                pollfd entry;
                entry.fd      = children[index].output_fd;
                entry.events  = POLLIN;
                entry.revents = 0;
                fds.push_back(entry);
                owners.push_back(index);
            }
            if (fds.empty())
                break;

            const int ready = ::poll(fds.data(), fds.size(), -1);
            if (ready < 0) {
                if (errno == EINTR)
                    continue; // a forwarded signal; children get it too
                break;
            }

            for (std::size_t slot = 0; slot < fds.size(); ++slot) {
                if ((fds[slot].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
                    continue;
                Child&                 child = children[owners[slot]];
                std::array<char, 4096> buffer;
                const ssize_t          got = ::read(child.output_fd, buffer.data(), buffer.size());
                if (got > 0) {
                    const std::string_view chunk(buffer.data(), static_cast<std::size_t>(got));
                    if (options.open_first_url && !opened_a_url) {
                        // Scan the chunk *together with* whatever partial line
                        // is still pending, so a URL split across two reads is
                        // still found. Reads land on arbitrary boundaries; a
                        // dev server's banner arriving in two pieces is
                        // ordinary, not exotic.
                        const std::string candidate = child.pending + std::string(chunk);
                        const std::string url       = find_url(candidate);
                        if (!url.empty()) {
                            opened_a_url = true;
                            const bool launched =
                                options.open_url ? options.open_url(url) : open_url(url);
                            if (launched) {
                                out_stream(options) << "kap: opened " << url << "\n";
                            } else {
                                out_stream(options)
                                    << "kap: found " << url
                                    << " but neither xdg-open nor open is available\n";
                            }
                        }
                    }
                    emit_labelled(child, chunk, options);
                } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
                    flush_pending(child, options);
                    ::close(child.output_fd);
                    child.output_fd = -1;
                }
            }
        }

        for (Child& child : children) {
            if (child.output_fd >= 0) {
                flush_pending(child, options);
                ::close(child.output_fd);
                child.output_fd = -1;
            }
            if (child.spawn_failed) {
                err_stream(options) << "kap: error: " << child.spawn_error << "\n";
                if (outcome.ok()) {
                    outcome.spawn_failed = true;
                    outcome.spawn_error  = child.spawn_error;
                    outcome.exit_code    = 127;
                }
                continue;
            }
            int status = 0;
            while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
            }
            const int code = status_to_exit_code(status);
            if (code != 0 && outcome.ok())
                outcome.exit_code = code;
        }
        return outcome;
    }

    // --- sequential ---------------------------------------------------------
    for (const kpl::Step& step : spec.steps) {
        if (options.verbose) {
            err_stream(options) << (options.color ? kDim : "")
                                << "kap: run: " << render_step(step, options)
                                << (options.color ? kReset : "") << "\n";
        }

        Child child = spawn_process(step.command, step.cwd, step.environment, root, false);
        if (child.spawn_failed) {
            err_stream(options) << "kap: error: " << child.spawn_error << "\n";
            outcome.spawn_failed = true;
            outcome.spawn_error  = child.spawn_error;
            outcome.exit_code    = 127;
            return outcome;
        }

        track_child(child.pid);
        int status = 0;
        while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
        }
        untrack_children();

        const int code = status_to_exit_code(status);
        if (code != 0) {
            // Stop here. A failed configure has not earned its build; running
            // on would bury the error that matters under a second one.
            outcome.exit_code = code;
            return outcome;
        }
    }

    if (spec.report_freed_space) {
        const std::uintmax_t after = total_usage(measured);
        const std::uintmax_t freed = after < before ? before - after : 0;
        out_stream(options) << "kap: freed " << human_bytes(freed) << "\n";
    }

    return outcome;
}

Outcome run_hook(const std::string& command, const std::string& name, const Options& options)
{
    Outcome outcome;
    if (command.empty())
        return outcome;

    // §5.13: "Hooks are plain shell strings run by the executor, not KPL."
    // A shell is correct *here* and nowhere else: the string is the user's own
    // kap.toml, and `echo a && echo b` is only meaningful with one. Note the
    // argv array is still explicit — this is sh -c with an argument, never
    // system().
    const std::vector<std::string> argv_words = {"/bin/sh", "-c", command};

    if (options.dry_run) {
        out_stream(options) << (options.color ? kDim : "") << "$ [" << name << "] "
                            << (options.color ? kReset : "") << command << '\n';
        return outcome;
    }
    if (options.verbose)
        err_stream(options) << "kap: hook " << name << ": " << command << "\n";

    const std::filesystem::path root =
        options.root.empty() ? std::filesystem::current_path() : options.root;

    SignalGuard guard;
    Child       child = spawn_process(argv_words, std::nullopt, {}, root, false);
    if (child.spawn_failed) {
        err_stream(options) << "kap: error: hook " << name << ": " << child.spawn_error << "\n";
        outcome.spawn_failed = true;
        outcome.spawn_error  = child.spawn_error;
        outcome.exit_code    = 127;
        return outcome;
    }

    track_child(child.pid);
    int status = 0;
    while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
    }
    untrack_children();

    outcome.exit_code = status_to_exit_code(status);
    if (outcome.exit_code != 0) {
        err_stream(options) << "kap: error: hook " << name << " failed with exit status "
                            << outcome.exit_code << "\n";
    }
    return outcome;
}

} // namespace exec
} // namespace kap
