// core/help.cpp
//
// The per-command help pages declared in core/help.hpp.
//
// These are long on purpose. A CLI's help is the documentation most people
// actually read, and the ones that fit on four lines are the ones that send you
// to a web page instead. Every page here says what the command does, what its
// options mean, and what a real invocation looks like — and, where a command
// has a genuine gotcha (`install`'s two meanings, `dev -o`'s cost, `config
// set`'s effect on comments), says that too rather than letting it be
// discovered.

#include "core/help.hpp"

#include <algorithm>

namespace kap
{
namespace help
{

namespace
{

// Shared by every project command's page, because the answer is the same for
// all twelve and repeating it twelve times would guarantee twelve versions.
constexpr std::string_view kProjectCommandTail = R"(
OPTIONS
  -n, --dry-run      Print the commands instead of running them. Always safe,
                     and the fastest way to see what a plugin decided.
      --root <path>  Use that directory instead of the current one.
      --set k=v      Override one of the plugin's configuration keys, for this
                     run only. Repeatable. See 'kap help config'.
      --verbose      Narrate every stage: which configuration files were read,
                     which plugins were considered and why one won, the
                     resolved configuration, and each step before it runs.
  -h, --help         This page.

TOOL ARGUMENTS
  Anything after '--' is passed to the underlying tool, not to kap:

      kap build -- --target install
      kap test  -- --nocapture

  A bare tool argument is refused rather than guessed at, so that
  'kap build --release' cannot come to mean two different things on the day
  kap grows a --release of its own.

WHICH PLUGIN RUNS
  Whichever one claims this directory. 'kap detect' shows you which, and why.
  If the matched plugin does not define this command, kap says so and lists
  the ones it does define.
)";

// Built once and returned by reference; the strings are all literals, so this
// costs one vector construction per process at most.
const std::vector<Topic>& all_topics()
{
    static const std::vector<Topic> topics = {
        // --- project commands ---------------------------------------------
        {"build",
         "compile the project",
         R"(kap build — compile the project

SYNOPSIS
  kap build [options] [-- <tool arguments>]

DESCRIPTION
  Runs whatever the plugin that claims this directory means by "build".

      cmake-cpp    mkdir -p build, cmake -S . -B build ..., cmake --build build
      cargo-rust   cargo build [--release]
      go           go build ./...
      node         npm run build  (or pnpm/yarn/bun, from the lockfile)

  kap contains none of that knowledge; it is all declared in plugin.kpl files.

EXAMPLES
  kap build                          Build it.
  kap build -n                       Show the commands without running them.
  kap build --set build_dir=out      Build somewhere else, once.
  kap build -- --target install      Pass --target install to the build tool.
)"},

        {"check",
         "typecheck without producing artifacts",
         R"(kap check — check the project without building it

SYNOPSIS
  kap check [options] [-- <tool arguments>]

DESCRIPTION
  The cheapest thing that catches a mistake. What that means depends on the
  ecosystem:

      cargo-rust   cargo check
      go           go vet ./...
      cmake-cpp    the configure step only — enough to catch a broken
                   CMakeLists.txt, an unfindable dependency, or a missing
                   toolchain
      python-uv    the configured type checker, or ruff check

EXAMPLES
  kap check
  kap check -n
)"},

        {"test",
         "run the tests",
         R"(kap test — run the tests

SYNOPSIS
  kap test [options] [-- <tool arguments>]

DESCRIPTION
  Runs the project's test suite through whichever plugin claims the directory.

EXAMPLES
  kap test
  kap test -- --nocapture            Arguments after -- reach the test runner.
  kap test -- -R integration         ctest -R, for a cmake-cpp project.
)"},

        {"lint",
         "run the linter",
         R"(kap lint — run the linter

SYNOPSIS
  kap lint [options] [-- <tool arguments>]

DESCRIPTION
  Runs the ecosystem's linter: clippy, golangci-lint (or go vet), ruff, eslint
  via the package manager, or clang-tidy.

  Some plugins need configuration first. cmake-cpp's lint reads the compilation
  database CMake writes and needs 'format_glob' set to know which files are
  yours; a CMake tree routinely contains vendored sources that must not be
  reported on.

EXAMPLES
  kap lint
  kap lint -n
)"},

        {"fmt",
         "format the source",
         R"(kap fmt — format the source

SYNOPSIS
  kap fmt [options] [-- <tool arguments>]

DESCRIPTION
  Rewrites the source in place with the ecosystem's formatter.

  To *check* formatting instead of changing it — which is what a CI job wants —
  set the plugin's 'check' key:

      kap fmt --set check=true

  'kap ci' does that for you when the plugin's schema declares a boolean
  'check' field. That is the difference between a pipeline that verifies
  formatting and one that silently rewrites your checkout.

EXAMPLES
  kap fmt
  kap fmt --set check=true
)"},

        {"run",
         "run the project",
         R"(kap run — run the project

SYNOPSIS
  kap run [options] [-- <program arguments>]

DESCRIPTION
  Runs whatever the project produces.

  Some ecosystems cannot tell kap which binary you meant. cmake-cpp requires
  'run_target' rather than guessing:

      kap run --set run_target=demo

  A wrong guess is worse than a clear question.

EXAMPLES
  kap run
  kap run -- --port 8080             Arguments after -- reach your program.
)"},

        {"dev",
         "run the development loop",
         R"(kap dev — run the development loop

SYNOPSIS
  kap dev [-o|--open] [options]

DESCRIPTION
  Starts the project's development loop. In a workspace this usually means
  several processes at once: kap runs them concurrently and prefixes every line
  of output with the package that produced it.

      packages/api | listening on :3001
      packages/web | vite v5.4.2  ready in 243 ms

  Ctrl-C stops all of them.

OPTIONS
  -o, --open         Open the first URL any step prints, once.

                     This has a cost worth knowing before it surprises you: to
                     find a URL kap has to *read* the output, which means piping
                     it, which means a tool that checks whether it is talking to
                     a terminal will drop its colours. Use -o when you want the
                     browser; leave it off when you want the prettier output.

EXAMPLES
  kap dev
  kap dev -o
  kap dev -n                         Show what would start, and where.
)"},

        {"clean",
         "remove build output",
         R"(kap clean — remove build output

SYNOPSIS
  kap clean [options]

DESCRIPTION
  Removes what the build produced, and reports how much space that recovered.

  Plugins deliberately leave the expensive-to-restore directories alone:
  node_modules and .venv are technically build output, but deleting them turns
  a five-second clean into a five-minute reinstall. Add them yourself if that is
  what you want:

      [plugins.node]
      clean_paths = ["dist", "node_modules"]

EXAMPLES
  kap clean
  kap clean -n                       See exactly what would be removed.
)"},

        {"install",
         "install the project — or a plugin",
         R"(kap install — install the project, or install a plugin

SYNOPSIS
  kap install                        Install the PROJECT.
  kap install <plugin|url>           Install a PLUGIN. Same as 'kap plugin install'.

DESCRIPTION
  This command means two things, and which one you get depends on whether you
  named something.

  With no arguments it is one of kap's project commands: it installs the
  project in this directory, the way 'cargo install --path .',
  'cmake --install', 'go install', or 'npm install' would.

  With an argument it installs a *plugin*, because that is what everyone means
  when they type it, and because the project command takes no positional
  arguments at all — so 'kap install <word>' had no other possible meaning.
  It is exactly 'kap plugin install <word>'; see 'kap help plugin install'.

EXAMPLES
  kap install                        Install this project.
  kap install -- --prefix=/opt       ...passing --prefix to the tool.
  kap install cmake-cpp              Install the cmake-cpp plugin.
  kap install https://example.com/kap-zig/install.sh
                                     Install a third-party plugin.
)"},

        {"ci",
         "fmt-check, lint, and test",
         R"(kap ci — the checks a pipeline should run

SYNOPSIS
  kap ci [options]

DESCRIPTION
  One command for a CI job.

  If the matched plugin defines its own 'ci' command, that is what runs — and
  most first-party plugins do, because the right order and flags are
  ecosystem-specific.

  Otherwise kap composes it: whichever of 'fmt', 'lint', and 'test' the plugin
  defines, in that order, stopping at the first failure. For the 'fmt' phase
  only, if the plugin's schema declares a boolean 'check' field, kap sets it —
  so CI verifies formatting rather than rewriting your checkout.

  'kap ci' takes no tool arguments: they would mean something different to each
  phase.

EXAMPLES
  kap ci
  kap ci -n                          Show every phase's commands.
)"},

        {"doctor",
         "check the tools this project needs",
         R"(kap doctor — check that the tools this project needs are installed

SYNOPSIS
  kap doctor [options]

DESCRIPTION
  Collects what every matched plugin declares in its 'requires' block and
  reports whether each tool is on your PATH.

      kap doctor
        plugin   cmake-cpp
        ok       cmake
        ok       ninja  (optional)
        --       ccache  (optional, not installed)
      healthy

  Exits 0 when healthy and 1 when a *required* tool is missing, so it works as
  a CI gate and not only as something to read. A missing optional tool is
  reported but does not fail: ccache not being installed is a slower build, not
  a broken one.

  doctor is itself a plugin, written in KPL. kap collects the requirements and
  hands them over; the plugin decides what to print and what counts as healthy.

OPTIONS
      --set strict=true  Make a missing optional tool a failure too.

EXAMPLES
  kap doctor
  kap doctor --set strict=true
)"},

        {"ports",
         "show what is listening locally",
         R"(kap ports — show which local ports are listening

SYNOPSIS
  kap ports [options] [-- <tool arguments>]

DESCRIPTION
  Shows what is listening, and what is holding it, using whichever of ss, lsof,
  or netstat you have.

  'kap ports -n' prints the exact command first, which is the quickest way to
  see what kap chose.

OPTIONS
      --set tool=ss|lsof|netstat   Choose the tool instead of detecting it.
      --set udp=true               Include UDP as well as TCP.
      --set listening_only=false   Include established connections.
      --set show_process=false     Skip the owning-process column.

EXAMPLES
  kap ports
  kap ports -n
  kap ports --set udp=true
)"},

        // --- kap's own commands --------------------------------------------
        {"detect",
         "show which plugin claims this directory",
         R"(kap detect — show which plugin claims this directory, and why

SYNOPSIS
  kap detect [--refresh] [--root <path>]

DESCRIPTION
  The command to run when kap does something you did not expect. It prints the
  winning plugin, the files that made it match, where the plugin came from, and
  whether the answer was cached.

      cmake-cpp  priority=30 score=1
        markers: CMakeLists.txt
        source: bundled (/usr/local/share/kap/plugins/cmake-cpp)
        root:   /home/you/code/demo
        cache:  hit

  priority decides who wins when several plugins match. score is how many of
  that plugin's rules fired — detail, not a tiebreaker.

  A plugin marked (composable) rides alongside the winner instead of competing
  with it; doctor and ports claim every directory that way. If *only* composable
  plugins matched, nothing owns the directory and detect exits 1 — saying so is
  more useful than naming a sidecar as the owner.

  Exits 0 when a plugin claims the directory, 1 when none does, and 1 with an
  explanation when two match at the same priority and kap refuses to guess.

OPTIONS
  -r, --refresh      Ignore .kap/cache.json and rescan.
      --root <path>  Look at that directory instead.

EXAMPLES
  kap detect
  kap detect --refresh
  kap detect --root ../other-project
)"},

        {"config",
         "read and write configuration",
         R"(kap config — read and write kap's configuration

SYNOPSIS
  kap config get [--global|--project] <key>
  kap config set [--global|--project] <key> <value>
  kap config edit [--global|--project]

DESCRIPTION
  Configuration comes in layers, later winning:

      plugin schema defaults
        -> ~/.config/kap/config.toml     yours, on this machine
        -> ./kap.toml                    this project's, committed
        -> --set key=value               this invocation

  'get' reads the merged result by default, because "what will kap actually do"
  is the question you have. --global and --project narrow it to one file.

  'set' writes one file — ./kap.toml unless you pass --global. It infers the
  type from what you type: true/false become booleans, a plain number becomes an
  integer, everything else stays a string.

  'set' rewrites the file through kap's TOML writer, which round-trips values
  but NOT comments or layout. If your kap.toml is carefully commented, use
  'kap config edit'.

KEYS
  plugins.<name>.<key>   A plugin's own setting. Its README lists them all.
  detect.ecosystem       Pin which plugin owns this directory.
  detect.max_walk_up     How far up the tree to look for a project (default 0).
  hooks.pre_<command>    A shell command to run before <command>.
  hooks.post_<command>   ...and after, but only if it succeeded.

EXAMPLES
  kap config get plugins.cmake-cpp.generator
  kap config set plugins.cmake-cpp.build_dir out
  kap config set --global detect.max_walk_up 3
  kap config edit
  kap -n config set detect.max_walk_up 3      Show the write without doing it.
)"},

        {"completions",
         "print a shell completion script",
         R"(kap completions — print a shell completion script

SYNOPSIS
  kap completions <bash|zsh|fish>

DESCRIPTION
  Prints a completion script on stdout. The scripts are generated from the same
  command lists kap dispatches on, so they cannot drift from the binary that
  printed them.

INSTALLING
  bash    kap completions bash > ~/.local/share/bash-completion/completions/kap
  zsh     kap completions zsh  > "${fpath[1]}/_kap"
  fish    kap completions fish > ~/.config/fish/completions/kap.fish

  For zsh, restart the shell (or run compinit) afterwards.
)"},

        {"help",
         "show help for a command",
         R"(kap help — show help for a command

SYNOPSIS
  kap help [<command>]
  kap <command> --help

DESCRIPTION
  With no argument, lists every command and its one-line summary.
  With one, prints that command's full page.

  'kap <command> --help' is the same thing, and is what you will reach for.

EXAMPLES
  kap help
  kap help install
  kap help plugin install
  kap plugin install --help
)"},

        // --- plugin subcommands ---------------------------------------------
        {"plugin",
         "manage plugins",
         R"(kap plugin — manage plugins

SYNOPSIS
  kap plugin <subcommand> [options]

DESCRIPTION
  Everything to do with installing, inspecting, and authoring plugins. A plugin
  is what teaches kap about one ecosystem; the binary itself knows none.

SUBCOMMANDS
  list                       Show what is installed, and its state.
  search <query>             Search the registry index.
  install <name|url|path>    Install from the registry, a URL, or a directory.
  remove <name>              Uninstall.
  update [name]              Re-fetch one plugin, or all of them.
  enable | disable <name>    Switch a plugin off without uninstalling it.
  pin <name> <version>       Stop 'update' from moving it.
  new <name>                 Scaffold a new plugin.
  doctor [name|path]         Parse, validate, and type-check.
  test [name|path]           Run a plugin's fixture cases.

  Each has its own page: 'kap plugin install --help', and so on.

WHERE PLUGINS LIVE
  Searched in this order, so the first match wins:

    ./.kap/plugins/<name>/                 project-local, committed with the repo
    $KAP_PLUGIN_PATH                       colon-separated, for development
    ~/.local/share/kap/plugins/<name>/     kap plugin install
    <prefix>/share/kap/plugins/<name>/     installed alongside the binary
    ~/.cache/kap/embedded/<name>/          compiled into the binary, if this
                                           build has -DKAP_EMBED_PLUGINS=ON
    ./kap-plugins/<name>/                  a repo that develops plugins in-tree
    ./plugins/<name>/                      ...or the plainer name for the same
)"},

        {"plugin list",
         "show what is installed",
         R"(kap plugin list — show what is installed

SYNOPSIS
  kap plugin list [--verbose]

DESCRIPTION
  Every plugin kap can see, with its version, where the file was found, where it
  came from, and whether it is switched on.

      cmake-cpp     1.0.0  [bundled/local]
    ! node          1.2.0  [user/registry]    disabled
      my-thing      0.1.0  [user/link]        pinned=0.1.0

  The bracket is source/origin. Source is which directory it was found in
  (project, path, user, bundled, embedded, repo); origin is how it got there
  (registry, git, script, embedded, link, local). A leading '!' means disabled.

  --verbose adds each plugin's full path.
)"},

        {"plugin search",
         "search the registry index",
         R"(kap plugin search — search the registry index

SYNOPSIS
  kap plugin search <query>

DESCRIPTION
  Matches the query against every registry entry's name, description, and tags,
  so searching for "rust" finds cargo-rust whichever of the three the word is
  in. Matching is a plain lower-cased substring test, not fuzzy: a search that
  returns things you did not ask for is worse than one that returns nothing.

  The index is found in the first of these that exists:

      $KAP_REGISTRY
      ~/.local/share/kap/registry/index.toml
      <project>/registry/index.toml
      <prefix>/share/kap/registry/index.toml
      the copy compiled into this binary

  The last means search always works, even on a binary with nothing beside it.

  Exits 1 when nothing matches.

EXAMPLES
  kap plugin search rust
  kap plugin search build
)"},

        {"plugin install",
         "install a plugin",
         R"(kap plugin install — install a plugin

SYNOPSIS
  kap plugin install [options] <name|url|path>...
  kap plugin install [options] --bundle <bundle>

DESCRIPTION
  Installs a plugin from whichever of these the argument turns out to be,
  checked in this order:

  1. A plugin compiled into this binary (-DKAP_EMBED_PLUGINS=ON builds).
     Instant, offline, and exactly the code this kap ships.

  2. A name in the registry index. kap uses the entry's installer script when
     it has one — a URL it downloads and runs in a staging directory — and
     otherwise does a shallow git clone.

  3. An installer-script URL, for a plugin hosted anywhere:

         kap plugin install https://example.com/kap-zig/install.sh

     kap downloads it, shows you the URL, size, and SHA-256, tells you where it
     saved it so you can read it, and asks before running it. The script writes
     files into a staging directory; it decides nothing. Whatever it produces
     goes through the same validation as every other install.

     HTTPS only. kap runs what it downloads, and plain HTTP can be tampered
     with in transit. (Loopback is exempt, for testing.)

  4. A git URL: https://, git@, ssh://, git://, file://, or anything ending
     in .git.

  5. A local directory containing a plugin.kpl.

  Before anything is written, kap checks that the payload parses, that its
  manifest is complete, that its api_version is supported, that its detect
  rules are well-formed, and that every command type-checks — then verifies the
  SHA-256 against the registry index and refuses on a mismatch.

OPTIONS
  -y, --yes          Do not ask for confirmation.
      --link         Symlink a local directory instead of copying it. Edits to
                     your working copy take effect immediately; 'remove' later
                     removes the link and leaves the copy alone. This is how you
                     develop a plugin.
      --project      Install into ./.kap/plugins, which is committed with the
                     repository — so everyone who clones it gets the plugin
                     without installing anything.
      --force        Reinstall over an existing copy.
      --bundle <b>   Install a named set: core, system, cpp, web.

  Without --yes, a non-interactive stdin is treated as "no", never yes.
  Installing third-party code because nobody was there to object is the exact
  failure the prompt exists to prevent.

EXAMPLES
  kap plugin install cmake-cpp
  kap plugin install --bundle core
  kap plugin install --link ./my-plugin
  kap plugin install --project ./my-plugin
  kap plugin install https://example.com/kap-zig/install.sh
  kap plugin install https://github.com/someone/kap-zig
)"},

        {"plugin remove",
         "uninstall a plugin",
         R"(kap plugin remove — uninstall a plugin

SYNOPSIS
  kap plugin remove <name>

DESCRIPTION
  Deletes the plugin's directory and its row in the lockfile.

  A plugin installed with --link is a symlink: removing it removes the link and
  leaves your working copy exactly where it was.

  Exits 1 when there is no such installed plugin.

EXAMPLES
  kap plugin remove node
  kap -n plugin remove node          Show what would be deleted.
)"},

        {"plugin update",
         "re-fetch installed plugins",
         R"(kap plugin update — re-fetch installed plugins

SYNOPSIS
  kap plugin update [name]

DESCRIPTION
  Runs the install pipeline again for one plugin, or for every installed plugin
  when given no name, keeping each one's original source.

  Two are skipped, with a reason:

    pinned   'kap plugin pin' was used. Unpin it with --clear first.
    linked   A --link install is a symlink to your working copy, so it is
             always current and there is nothing to fetch.

  Naming a plugin explicitly makes those an error; updating everything reports
  them and carries on, because you did not ask about that one in particular.

EXAMPLES
  kap plugin update
  kap plugin update cmake-cpp
)"},

        {"plugin enable",
         "switch a plugin back on",
         R"(kap plugin enable — switch a plugin back on

SYNOPSIS
  kap plugin enable <name>

DESCRIPTION
  Undoes 'kap plugin disable'. See 'kap help plugin disable'.
)"},

        {"plugin disable",
         "switch a plugin off without uninstalling it",
         R"(kap plugin disable — switch a plugin off without uninstalling it

SYNOPSIS
  kap plugin disable <name>

DESCRIPTION
  A disabled plugin is skipped by detection, so it can never claim a directory —
  but its files stay where they are, 'kap plugin doctor' still checks it, and
  'kap plugin enable' brings it straight back.

  The state lives in the lockfile rather than in the filesystem, which is what
  makes it reversible and keeps a switched-off plugin inspectable.

  Useful when two plugins fight over the same project and you want one of them
  out of the way without deciding to delete it.

EXAMPLES
  kap plugin disable make-generic
  kap plugin enable make-generic
)"},

        {"plugin pin",
         "lock a plugin's version",
         R"(kap plugin pin — lock a plugin's version

SYNOPSIS
  kap plugin pin <name> <version>
  kap plugin pin <name> --clear

DESCRIPTION
  A pinned plugin is skipped by 'kap plugin update', which reports the pin
  rather than quietly ignoring it. Use it when a newer version broke something
  and you have not worked out why yet.

  --clear removes the pin.

EXAMPLES
  kap plugin pin node 1.0.0
  kap plugin pin node --clear
)"},

        {"plugin new",
         "scaffold a new plugin",
         R"(kap plugin new — scaffold a new plugin

SYNOPSIS
  kap plugin new <name> [--template build-system]

DESCRIPTION
  Writes a complete, working plugin you can edit:

      <name>/plugin.kpl                              the whole plugin
      <name>/README.md                               what it does, how to configure it
      <name>/tests/fixtures/example/                 a fake project
      <name>/tests/expected/example.build.steps.json the commands it should produce

  It is valid immediately — 'kap plugin doctor <name>' passes and
  'kap plugin test <name>' has a passing case — so the first thing you do is
  change something, not debug the template.

  Edit the 'detect' block to claim your projects and the 'command' blocks to run
  your tools, then:

      kap plugin doctor <name>            does it parse and type-check
      kap plugin test <name>              do its cases pass
      kap plugin install --link <name>    use it for real, live-edited

  See docs/plugins.md for the guide and docs/PLUGIN_API.md for the language.

EXAMPLES
  kap plugin new zig
  kap plugin new zig --template build-system
)"},

        {"plugin doctor",
         "parse, validate, and type-check plugins",
         R"(kap plugin doctor — check that plugins are well-formed

SYNOPSIS
  kap plugin doctor [<name|path>...]

DESCRIPTION
  With no argument, checks every plugin kap can see. With names or paths, checks
  those.

  A *path* works, so you can check a plugin before installing it — which is what
  you want right after 'kap plugin new'. Needing to install something before you
  could syntax-check it would be backwards.

  Each plugin is checked for everything that would stop it running: the file
  parses, the manifest has what an install requires, its api_version is one this
  kap supports, its detect rules are well-formed, and every command type-checks.
  A plugin whose manifest is fine but whose build command reads an undeclared
  config key is broken, and reporting it as healthy would be a lie.

  Exits 1 if any plugin fails.

EXAMPLES
  kap plugin doctor
  kap plugin doctor cmake-cpp
  kap plugin doctor ./my-plugin
)"},

        {"plugin test",
         "run a plugin's fixture cases",
         R"(kap plugin test — run a plugin's fixture cases

SYNOPSIS
  kap plugin test [<name|path>...]

DESCRIPTION
  Evaluates a plugin's command blocks against fixture project trees and compares
  the resulting command lists with committed golden files.

  Nothing is executed. That is the point: kap's own CI tests all eight bundled
  plugins with no cmake, cargo, npm, go, or uv installed anywhere. A case
  declares what project.tool() and project.env() should report, so a test cannot
  pass on your laptop and fail in CI.

  A failure prints both the expected and the actual command list in full,
  because "it differs" is not actionable.

  Layout:

      tests/fixtures/<fixture>/                        a fake project tree
      tests/expected/<fixture>.<command>.steps.json    one case

  See docs/plugins.md for the case-file format.

EXAMPLES
  kap plugin test
  kap plugin test node
  kap plugin test ./my-plugin
)"},
    };
    return topics;
}

} // namespace

const std::vector<Topic>& topics()
{
    return all_topics();
}

const Topic* find(std::string_view name)
{
    for (const Topic& topic : all_topics())
        if (topic.name == name)
            return &topic;
    return nullptr;
}

std::string page(std::string_view name)
{
    const Topic* topic = find(name);
    if (topic == nullptr)
        return {};

    std::string text(topic->body);
    // Project commands share their options and tool-argument sections, which
    // are identical for all twelve. Appending rather than repeating is what
    // keeps them from drifting into twelve slightly different explanations.
    static const std::vector<std::string_view> kProjectCommands = {
        "build", "check", "ci", "clean", "dev", "doctor", "fmt", "lint", "ports", "run", "test"};
    if (std::find(kProjectCommands.begin(), kProjectCommands.end(), name) !=
        kProjectCommands.end()) {
        text += kProjectCommandTail;
    }
    return text;
}

bool requested(const std::vector<std::string>& args)
{
    return std::any_of(args.begin(), args.end(), [](const std::string& argument) {
        return argument == "-h" || argument == "--help";
    });
}

std::vector<std::string> without_help_flags(const std::vector<std::string>& args)
{
    std::vector<std::string> kept;
    for (const std::string& argument : args)
        if (argument != "-h" && argument != "--help")
            kept.push_back(argument);
    return kept;
}

} // namespace help
} // namespace kap
