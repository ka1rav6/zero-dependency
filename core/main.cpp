// core/main.cpp
//
// kap — "know project, act". A zero-config CLI that detects what kind of
// project you're standing in and runs the right underlying tool for common
// tasks (build, test, lint, run, ...). See docs/design.md for the full design.
//
// This file is the wiring, not the work. Every subsystem lives in its own
// header and this one only decides what to call in which order:
//
//     core/cli.hpp     parse the command line
//     core/config.hpp  merge the configuration layers (§5.12)
//     core/plugin.hpp  find the plugins installed on this machine (§6.5)
//     core/detect.hpp  decide which of them owns this directory (§3)
//     core/kpl.hpp     type-check and evaluate its command block (§5)
//     core/exec.hpp    run the CommandSpec that produced (§4 step 6)
//
// §4's seven-step lifecycle is `run_project_command()` below, read top to
// bottom. Keeping it in one readable function is deliberate: when `kap build`
// does something surprising, that function is the whole explanation.

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <istream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/cli.hpp"
#include "core/config.hpp"
#include "core/detect.hpp"
#include "core/diag.hpp"
#include "core/exec.hpp"
#include "core/fs.hpp"
#include "core/kapc.hpp"
#include "core/kpl.hpp"
#include "core/paths.hpp"
#include "core/plugin.hpp"
#include "core/registry.hpp"
#include "core/toml.hpp"
#include "core/version.hpp"

namespace
{

// Print the canonical usage banner. The full command list stabilizes in later
// milestones; this stays accurate to what is actually implemented.
void print_usage(std::ostream& out)
{
    out << "usage: kap <command> [options] [-- <tool args>]\n"
           "       kap --version | --help\n"
           "\n"
           "project commands (handled by whichever plugin claims this directory):\n"
           "  build      compile the project\n"
           "  check      typecheck without producing artifacts\n"
           "  ci         fmt-check + lint + test, or whatever the plugin defines\n"
           "  clean      remove build output\n"
           "  dev        run the development loop (may run steps concurrently)\n"
           "  doctor     check that the tools the project needs are installed\n"
           "  fmt        format the source\n"
           "  install    install the project\n"
           "  lint       run the linter\n"
           "  ports      show what is listening locally\n"
           "  run        run the project\n"
           "  test       run the tests\n"
           "\n"
           "kap commands:\n"
           "  detect [--refresh]        show which plugin claims this directory\n"
           "  config get <key>          read one effective configuration value\n"
           "  config set <key> <value>  write one value to ./kap.toml\n"
           "  config edit               open a configuration file in $EDITOR\n"
           "  plugin ...                manage plugins ('kap plugin' for the list)\n"
           "\n"
           "global flags:\n"
           "  -n, --dry-run      print the commands instead of running them\n"
           "      --root <path>  search root for the project\n"
           "      --set k=v      override one plugin config key (repeatable)\n"
           "      --verbose      explain what kap is doing\n"
           "\n"
           "arguments for the underlying tool go after '--':\n"
           "  kap build -- --target install\n"
           "\n"
           "docs: docs/usage.md, docs/plugins.md, docs/design.md\n";
}

std::filesystem::path search_root(const kap::cli::GlobalOptions& global);

// `detect.ecosystem` from <root>/kap.toml (design doc §3.2 step 4): the pin
// that settles which plugin owns a directory when several match. Read here
// rather than inside the engine because the engine takes configuration as an
// input; Milestone 6's full config merge will supply the same value from the
// merged layers instead.
//
// A malformed kap.toml is not this function's problem to report — it will be
// reported properly by the config layer — so a parse failure yields "no pin".
std::string project_pin(const std::filesystem::path& root)
{
    const std::filesystem::path file = root / "kap.toml";
    if (!kap::fs::is_file(file))
        return {};
    try {
        const kap::toml::Document doc   = kap::toml::parse(kap::fs::read_text(file), file.string());
        const auto                value = doc.get("detect.ecosystem");
        if (value && value->kind == kap::toml::Value::Kind::String)
            return value->str;
    }
    catch (const kap::diag::Error&) {
    }
    return {};
}

// `kap detect` — resolve the project's matching plugin and explain the answer
// (design doc Milestone 4's debug subcommand).
//
// This is the one command whose whole job is to make the detection engine
// legible: which plugin won, how many of its rules fired, which files fired
// them, and whether the answer came from .kap/cache.json or a fresh scan.
// When detection *fails*, that transparency matters even more, so a failed
// run prints the candidates it considered and any warnings it collected.
int run_detect(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    bool refresh = false;
    for (const std::string& arg : args) {
        if (arg == "--refresh" || arg == "-r") {
            refresh = true;
            continue;
        }
        std::cerr << "kap: error: unknown option '" << arg << "' for 'kap detect'\n"
                  << "      note: usage: kap detect [--refresh]\n";
        return 2;
    }

    const std::filesystem::path root = search_root(global);

    kap::plugin::DiscoveryOptions discovery;
    discovery.project_root                          = root;
    const std::vector<kap::plugin::Located> plugins = kap::plugin::discover(discovery);

    kap::detect::Options options;
    options.ast_cache   = kap::kapc::cache_directory();
    options.read_cache  = !refresh;
    options.max_walk_up = 0;
    options.pinned      = project_pin(root);

    try {
        const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, options);

        for (const std::string& warning : resolution.warnings)
            std::cerr << "kap: warning: " << warning << "\n";

        if (!resolution.matched()) {
            std::cerr << "kap: error: no plugin claims " << resolution.root.string() << "\n";
            if (plugins.empty()) {
                std::cerr << "      note: no plugins are installed; try 'kap plugin install "
                             "--bundle core'\n";
            } else {
                std::cerr << "      note: considered:";
                for (const kap::plugin::Located& located : plugins)
                    std::cerr << ' ' << located.name;
                std::cerr << "\n";
            }
            return 1;
        }

        for (const kap::detect::Match& match : resolution.matches) {
            std::cout << match.name << "  priority=" << match.priority << " score=" << match.score
                      << (match.composable ? " (composable)" : "") << "\n";
            if (!match.matched_files.empty()) {
                std::cout << "  markers:";
                for (const std::string& file : match.matched_files)
                    std::cout << ' ' << file;
                std::cout << "\n";
            }
            std::cout << "  source: " << kap::plugin::source_name(match.located.source) << " ("
                      << match.located.directory.string() << ")\n";
        }
        std::cout << "  root:   " << resolution.root.string() << "\n";
        std::cout << "  cache:  " << (resolution.from_cache ? "hit" : "miss (rescanned)") << "\n";
        return 0;
    }
    catch (const kap::diag::Error& error) {
        std::cerr << error.report();
        return 1;
    }
}

// --- the project-command lifecycle (design doc §4) ---------------------------------

// The v1 command surface (§8). Everything here is dispatched to a plugin; the
// binary contains no ecosystem knowledge whatsoever, which is §3.1's whole
// point. `doctor` and `ports` are on this list because they too ship as KPL
// plugins (§4: "proving the DSL is expressive enough for system introspection,
// not just shelling out to a build tool").
const std::vector<std::string>& project_commands()
{
    static const std::vector<std::string> commands = {"build",
                                                      "check",
                                                      "ci",
                                                      "clean",
                                                      "dev",
                                                      "doctor",
                                                      "fmt",
                                                      "install",
                                                      "lint",
                                                      "ports",
                                                      "run",
                                                      "test"};
    return commands;
}

bool is_project_command(const std::string& name)
{
    const std::vector<std::string>& all = project_commands();
    return std::find(all.begin(), all.end(), name) != all.end();
}

// Everything one project command needs, assembled once.
struct Session
{
    std::filesystem::path             search_root;
    kap::config::Merged               config;
    std::vector<kap::plugin::Located> plugins;
    kap::detect::Resolution           resolution;
};

// Steps 1-3 of §4: settle the root, merge the configuration, find the plugins,
// and resolve which one owns this directory.
Session open_session(const kap::cli::GlobalOptions& global)
{
    Session session;
    session.search_root = search_root(global);
    session.config      = kap::config::load(session.search_root);

    if (global.verbose) {
        for (const kap::config::Layer& layer : session.config.layers)
            std::cerr << "kap: config: " << layer.name << " " << layer.file.string() << "\n";
    }
    for (const std::string& warning : session.config.warnings)
        std::cerr << "kap: warning: " << warning << "\n";

    // Discovery finds every plugin on disk; the lockfile is what remembers
    // which of them the user switched off (§6.1). Applying it here is what
    // makes `kap plugin disable` actually change what `kap build` does.
    kap::plugin::DiscoveryOptions discovery;
    discovery.project_root = session.search_root;
    session.plugins        = kap::plugin::discover(discovery);
    kap::registry::apply_lockfile(kap::registry::load_lockfile(kap::paths::lockfile()),
                                  session.plugins);

    kap::detect::Options options;
    options.ast_cache   = kap::kapc::cache_directory();
    options.max_walk_up = session.config.settings.max_walk_up;
    options.pinned      = session.config.settings.ecosystem;
    session.resolution  = kap::detect::resolve(session.search_root, session.plugins, options);
    return session;
}

// Explain a failed detection well enough to act on. "No plugin matched" alone
// leaves a new user with nowhere to go, so this distinguishes "you have no
// plugins" from "none of your plugins claims this directory" and prints any
// warning that might be the actual cause.
int report_no_match(const Session& session)
{
    std::cerr << "kap: error: no plugin claims " << session.resolution.root.string() << "\n";
    for (const std::string& warning : session.resolution.warnings)
        std::cerr << "      note: " << warning << "\n";
    if (session.plugins.empty()) {
        std::cerr << "      note: no plugins are installed\n"
                     "      note: try 'kap plugin install --bundle core'\n";
    } else {
        std::cerr << "      note: considered:";
        for (const kap::plugin::Located& located : session.plugins)
            std::cerr << ' ' << located.name;
        std::cerr << "\n      note: run 'kap detect' to see why none of them matched\n";
    }
    return 1;
}

// One plugin, loaded and ready to evaluate.
struct LoadedPlugin
{
    const kap::detect::Match* match = nullptr;
    kap::kpl::Plugin          ast;
};

bool defines_command(const kap::kpl::Plugin& plugin, const std::string& name)
{
    for (const kap::kpl::Command& command : plugin.commands)
        if (command.name == name)
            return true;
    return false;
}

// Find the plugin that should handle `command`.
//
// §3.3: a composable plugin "runs alongside another matched plugin (e.g. a
// docker-compose sidecar that adds `kap up`/`kap down` without claiming
// build/test)". So the primary plugin is asked first and a sidecar only gets a
// command the primary does not define — which is exactly what "without
// claiming build/test" means in code.
std::optional<LoadedPlugin> find_handler(const Session&            session,
                                         const std::string&        command,
                                         std::vector<std::string>& available)
{
    const std::filesystem::path ast_cache = kap::kapc::cache_directory();

    std::optional<LoadedPlugin> found;
    for (const kap::detect::Match& match : session.resolution.matches) {
        kap::kpl::Plugin ast;
        try {
            ast = kap::kapc::load(match.located.manifest, ast_cache).plugin;
        }
        catch (const kap::diag::Error&) {
            continue; // already warned about during detection
        }
        for (const kap::kpl::Command& declared : ast.commands) {
            if (std::find(available.begin(), available.end(), declared.name) == available.end())
                available.push_back(declared.name);
        }
        if (!found && defines_command(ast, command)) {
            LoadedPlugin loaded;
            loaded.match = &match;
            loaded.ast   = std::move(ast);
            found        = std::move(loaded);
            // Not returning yet: the loop also collects the full command list
            // used by the "no such command" message below, and a sidecar's
            // commands belong in it.
        }
    }
    std::sort(available.begin(), available.end());
    return found;
}

// Build the exec options every step and hook shares.
kap::exec::Options executor_options(const kap::cli::GlobalOptions& global,
                                    const std::filesystem::path&   root)
{
    kap::exec::Options options;
    options.root    = root;
    options.dry_run = global.dry_run;
    options.verbose = global.verbose;
    options.color   = kap::exec::default_color();
    return options;
}

// Evaluate one command block into a CommandSpec (§4 steps 4-5).
//
// Returns nullopt after printing the reason: a plugin that does not type-check
// or a configuration key that does not exist are both user-fixable problems,
// and reporting *every* such problem at once beats one per run.
std::optional<kap::kpl::CommandSpec>
evaluate_command(const Session&                                session,
                 const LoadedPlugin&                           loaded,
                 const std::string&                            command,
                 const kap::cli::GlobalOptions&                global,
                 const std::vector<std::string>&               extra,
                 const std::map<std::string, kap::kpl::Value>& injected)
{
    const std::string& name = loaded.match->name;

    const std::vector<std::string> type_errors = kap::kpl::type_check(loaded.ast);
    if (!type_errors.empty()) {
        std::cerr << "kap: error: plugin '" << name << "' does not type-check\n";
        for (const std::string& error : type_errors)
            std::cerr << "      note: " << error << "\n";
        std::cerr << "      note: " << loaded.match->located.manifest.string() << "\n";
        return std::nullopt;
    }

    kap::config::PluginConfig plugin_config =
        kap::config::for_plugin(loaded.ast, name, session.config, global.set_values);
    for (const auto& [key, value] : injected)
        plugin_config.values[key] = value;

    if (!plugin_config.errors.empty()) {
        std::cerr << "kap: error: configuration for plugin '" << name << "' is not usable\n";
        for (const std::string& error : plugin_config.errors)
            std::cerr << "      note: " << error << "\n";
        return std::nullopt;
    }

    try {
        const kap::kpl::Project project =
            kap::kpl::host_project(session.resolution.root.string(), loaded.match->matched_files);
        return kap::kpl::evaluate(loaded.ast, command, project, plugin_config.values, extra);
    }
    catch (const kap::diag::Error& error) {
        std::cerr << error.report();
        return std::nullopt;
    }
}

// Run `pre_<command>` / `post_<command>` from kap.toml (§5.13).
int run_hook_phase(const Session&            session,
                   const std::string&        phase,
                   const std::string&        command,
                   const kap::exec::Options& options)
{
    const std::optional<std::string> hook = session.config.hook(phase, command);
    if (!hook)
        return 0;
    return kap::exec::run_hook(*hook, phase + "_" + command, options).exit_code;
}

// One command, end to end: find its plugin, evaluate it, run the hooks and the
// steps. Returns the exit status kap should exit with.
int dispatch_one(const Session&                                session,
                 const std::string&                            command,
                 const kap::cli::GlobalOptions&                global,
                 const std::vector<std::string>&               extra,
                 const std::map<std::string, kap::kpl::Value>& injected = {})
{
    std::vector<std::string>          available;
    const std::optional<LoadedPlugin> loaded = find_handler(session, command, available);

    if (!loaded) {
        const kap::detect::Match* primary = session.resolution.primary();
        std::cerr << "kap: error: no matched plugin defines a '" << command << "' command\n";
        if (primary != nullptr)
            std::cerr << "      note: '" << primary->name << "' claims "
                      << session.resolution.root.string() << "\n";
        if (available.empty()) {
            std::cerr << "      note: it defines no commands at all\n";
        } else {
            std::cerr << "      note: available:";
            for (const std::string& name : available)
                std::cerr << ' ' << name;
            std::cerr << "\n";
        }
        return 1;
    }

    const std::optional<kap::kpl::CommandSpec> spec =
        evaluate_command(session, *loaded, command, global, extra, injected);
    if (!spec)
        return 1;

    const kap::exec::Options options = executor_options(global, session.resolution.root);

    if (const int hook_status = run_hook_phase(session, "pre", command, options); hook_status != 0)
        return hook_status;

    const kap::exec::Outcome outcome = kap::exec::run(*spec, options);
    if (!outcome.ok())
        return outcome.exit_code;

    // The post hook runs only on success. A `post_test = "notify-send 'tests
    // finished'"` firing after a failed test run would be actively misleading,
    // and a user who wants "always" can put it in the pre hook of whatever
    // comes next.
    return run_hook_phase(session, "post", command, options);
}

// `kap ci` — §8: "fmt-check + lint + test, or plugin-defined".
//
// A plugin that declares its own `ci` command owns the meaning entirely. When
// none does, kap composes the three phases §8 names, skipping any the plugin
// does not define, and stopping at the first failure.
//
// "fmt-check" rather than "fmt" is implemented literally: if the plugin's
// schema declares a bool field named `check`, kap sets it for the fmt phase
// only. That is the difference between a CI job *verifying* formatting and one
// silently rewriting the checkout — and it is what the bundled plugins' `check`
// field already exists for (see cargo-rust's `fmt`).
int dispatch_ci(const Session&                  session,
                const kap::cli::GlobalOptions&  global,
                const std::vector<std::string>& extra)
{
    std::vector<std::string>          available;
    const std::optional<LoadedPlugin> declared = find_handler(session, "ci", available);
    if (declared)
        return dispatch_one(session, "ci", global, extra);

    bool ran_anything = false;
    for (const char* phase : {"fmt", "lint", "test"}) {
        std::vector<std::string>          phase_available;
        const std::optional<LoadedPlugin> handler = find_handler(session, phase, phase_available);
        if (!handler)
            continue;

        std::map<std::string, kap::kpl::Value> injected;
        if (std::string(phase) == "fmt") {
            for (const kap::kpl::SchemaField& field : kap::kpl::schema(handler->ast)) {
                if (field.name == "check" && field.type == "bool")
                    injected["check"] = kap::kpl::Value::boolean_value(true);
            }
        }

        if (global.verbose || !global.dry_run)
            std::cerr << "kap: ci: " << phase << "\n";
        ran_anything = true;

        // Passthrough arguments would mean something different to each phase,
        // so they are not forwarded; `kap ci` takes none.
        if (const int status = dispatch_one(session, phase, global, {}, injected); status != 0)
            return status;
    }

    if (!ran_anything) {
        std::cerr << "kap: error: nothing to do for 'ci'\n"
                     "      note: the matched plugin defines none of: ci, fmt, lint, test\n";
        return 1;
    }
    return 0;
}

// `kap <command>` for every command in §8's table.
int run_project_command(const std::string& command, const kap::cli::Invocation& inv)
{
    if (!inv.argv.empty()) {
        // Arguments for the *underlying tool* go after `--` (§4: `kap build --
        // --release`). Accepting them bare would make `kap build --release`
        // and `kap build -- --release` both work but mean different things the
        // day kap grows a `--release` of its own.
        std::cerr << "kap: error: unexpected argument '" << inv.argv.front() << "'\n"
                  << "      note: arguments for the underlying tool go after '--':\n"
                  << "      note:     kap " << command << " --";
        for (const std::string& argument : inv.argv)
            std::cerr << ' ' << argument;
        std::cerr << "\n";
        return 2;
    }

    Session session;
    try {
        session = open_session(inv.global);
    }
    catch (const kap::diag::Error& error) {
        std::cerr << error.report();
        return 1;
    }

    if (inv.global.verbose) {
        for (const std::string& warning : session.resolution.warnings)
            std::cerr << "kap: warning: " << warning << "\n";
    }
    if (!session.resolution.matched())
        return report_no_match(session);

    if (command == "ci")
        return dispatch_ci(session, inv.global, inv.passthrough);
    return dispatch_one(session, command, inv.global, inv.passthrough);
}

// --- the plugin manager CLI (design doc §6.1, Milestone 7) ---------------------------

// The lockfile for this machine (§6.4).
kap::registry::Lockfile open_lockfile()
{
    return kap::registry::load_lockfile(kap::paths::lockfile());
}

// Discovery with the lockfile's enable/disable state already applied, so every
// caller sees one list that knows what is switched off.
std::vector<kap::plugin::Located> discover_with_lockfile(const std::filesystem::path&   root,
                                                         const kap::registry::Lockfile& lock)
{
    kap::plugin::DiscoveryOptions options;
    options.project_root                    = root;
    std::vector<kap::plugin::Located> found = kap::plugin::discover(options);
    kap::registry::apply_lockfile(lock, found);
    return found;
}

// Ask the user to confirm, as §7 requires ("prints a summary and requires
// confirmation unless --yes").
//
// A non-interactive stdin — a CI job, a pipeline — is treated as "no" rather
// than "yes". Installing third-party code because nobody was there to object
// is exactly the failure mode the prompt exists to prevent; a script that
// means it passes --yes.
bool confirm_install(const std::string& summary)
{
    std::cout << summary << "\nProceed? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::cout << "\n";
        std::cerr << "kap: error: cannot ask for confirmation (stdin is not interactive)\n"
                     "      note: pass --yes to install without confirming\n";
        return false;
    }
    return answer == "y" || answer == "Y" || answer == "yes";
}

// `kap plugin list` — §6.1.
int run_plugin_list(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (!args.empty()) {
        std::cerr << "kap: usage: kap plugin list\n";
        return 2;
    }
    const std::filesystem::path             root  = search_root(global);
    const kap::registry::Lockfile           lock  = open_lockfile();
    const std::vector<kap::plugin::Located> found = discover_with_lockfile(root, lock);

    if (found.empty()) {
        std::cout << "no plugins installed\n"
                     "  try 'kap plugin install --bundle core', or 'kap plugin new <name>' to "
                     "write one\n";
        return 0;
    }

    for (const kap::plugin::Located& located : found) {
        const auto        row     = lock.plugins.find(located.name);
        const std::string version = row != lock.plugins.end()
                                        ? row->second.version
                                        : kap::registry::declared_version(located.directory);
        const std::string origin =
            row != lock.plugins.end() ? kap::registry::origin_name(row->second.origin) : "local";

        std::cout << (located.enabled ? "  " : "! ") << located.name << "  " << version << "  ["
                  << kap::plugin::source_name(located.source) << "/" << origin << "]";
        if (!located.enabled)
            std::cout << "  disabled";
        if (row != lock.plugins.end() && !row->second.pinned.empty())
            std::cout << "  pinned=" << row->second.pinned;
        std::cout << "\n";
        if (global.verbose)
            std::cout << "      " << located.directory.string() << "\n";
    }
    return 0;
}

// `kap plugin search <query>` — §6.1. Matches the name, description, and tags,
// because a user looking for "rust" should find `cargo-rust` whichever of the
// three the word happens to be in.
int run_plugin_search(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (args.size() != 1) {
        std::cerr << "kap: usage: kap plugin search <query>\n";
        return 2;
    }
    const std::filesystem::path root = search_root(global);

    std::optional<kap::registry::Index> index;
    try {
        index = kap::registry::load_index(root);
    }
    catch (const kap::diag::Error& error) {
        std::cerr << error.report();
        return 1;
    }
    if (!index) {
        std::cerr << "kap: error: no registry index found\n"
                     "      note: looked for $KAP_REGISTRY, "
                  << (kap::paths::registry_dir().empty()
                          ? std::string("~/.local/share/kap/registry/index.toml")
                          : (kap::paths::registry_dir() / "index.toml").string())
                  << ",\n      note: and " << (root / "registry" / "index.toml").string() << "\n";
        return 1;
    }

    // Lower-cased substring matching. Not fuzzy: a registry is small, and a
    // search that returns things the user did not ask for is worse than one
    // that returns nothing and lets them try a shorter word.
    std::string needle = args[0];
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto contains_needle = [&needle](std::string haystack) {
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return haystack.find(needle) != std::string::npos;
    };

    int matches = 0;
    for (const auto& [name, entry] : index->plugins) {
        bool matched = contains_needle(name) || contains_needle(entry.description);
        for (const std::string& tag : entry.tags)
            matched = matched || contains_needle(tag);
        if (!matched)
            continue;
        ++matches;
        std::cout << "  " << name << "  " << entry.version << "\n";
        if (!entry.description.empty())
            std::cout << "      " << entry.description << "\n";
    }

    if (matches == 0) {
        std::cout << "no plugin in " << index->file.string() << " matches '" << args[0] << "'\n";
        return 1;
    }
    return 0;
}

// `kap plugin install <name|git-url|path>` — §6.1 and §6.3.
int run_plugin_install(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    kap::registry::InstallRequest request;
    request.project_root = search_root(global);

    std::string              bundle;
    std::vector<std::string> sources;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--yes" || arg == "-y") {
            request.assume_yes = true;
        } else if (arg == "--link") {
            request.link = true;
        } else if (arg == "--force") {
            request.force = true;
        } else if (arg == "--project") {
            request.project_local = true;
        } else if (arg == "--global") {
            request.project_local = false;
        } else if (arg == "--bundle") {
            if (index + 1 >= args.size()) {
                std::cerr << "kap: error: --bundle requires a bundle name\n";
                return 2;
            }
            bundle = args[++index];
        } else if (arg.starts_with("--bundle=")) {
            bundle = arg.substr(std::string("--bundle=").size());
        } else if (arg.size() > 1 && arg[0] == '-') {
            std::cerr << "kap: error: unknown option '" << arg << "' for 'kap plugin install'\n"
                      << "      note: expected --yes, --link, --force, --project, --bundle\n";
            return 2;
        } else {
            sources.push_back(arg);
        }
    }

    std::optional<kap::registry::Index> index;
    try {
        index = kap::registry::load_index(request.project_root);
    }
    catch (const kap::diag::Error& error) {
        std::cerr << error.report();
        return 1;
    }

    if (!bundle.empty()) {
        if (!sources.empty()) {
            std::cerr << "kap: error: --bundle installs a named set; do not also name plugins\n";
            return 2;
        }
        if (!index) {
            std::cerr << "kap: error: --bundle needs a registry index, and none was found\n";
            return 1;
        }
        const kap::registry::Bundle* found = index->find_bundle(bundle);
        if (found == nullptr) {
            std::cerr << "kap: error: no bundle named '" << bundle << "'\n";
            if (!index->bundles.empty()) {
                std::cerr << "      note: available:";
                for (const auto& [name, unused] : index->bundles) {
                    (void) unused;
                    std::cerr << ' ' << name;
                }
                std::cerr << "\n";
            }
            return 1;
        }
        sources = found->plugins;
    }

    if (sources.empty()) {
        std::cerr << "kap: usage: kap plugin install [--yes] [--link] [--project] "
                     "<name|git-url|path>\n"
                     "      note:  kap plugin install --bundle <name>\n";
        return 2;
    }

    kap::registry::Lockfile lock = open_lockfile();

    // A bundle installs several plugins and one failure should not abandon the
    // rest — the user asked for a set, and getting five of six with a clear
    // report beats getting two and an abort.
    int failures = 0;
    for (const std::string& source : sources) {
        kap::registry::InstallRequest one = request;
        one.source                        = source;

        if (global.dry_run) {
            std::cout << "would install '" << source << "' into "
                      << (one.project_local ? (one.project_root / ".kap" / "plugins").string()
                                            : kap::paths::user_plugin_dir().string())
                      << "\n";
            continue;
        }

        kap::registry::InstallResult result;
        try {
            result = kap::registry::install(
                one,
                lock,
                index,
                [&](const std::string& summary) {
                    return one.assume_yes ? true : confirm_install(summary);
                },
                global.verbose);
        }
        catch (const kap::diag::Error& error) {
            std::cerr << error.report();
            ++failures;
            continue;
        }

        if (result.installed) {
            std::cout << "installed " << result.name << " " << result.version << " -> "
                      << result.directory.string() << "\n";
        } else {
            std::cerr << "kap: error: " << result.message << "\n";
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

// `kap plugin remove <name>` — §6.1.
int run_plugin_remove(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (args.size() != 1) {
        std::cerr << "kap: usage: kap plugin remove <name>\n";
        return 2;
    }
    kap::registry::Lockfile lock = open_lockfile();

    if (global.dry_run) {
        const auto found = lock.plugins.find(args[0]);
        if (found == lock.plugins.end()) {
            std::cerr << "kap: error: '" << args[0] << "' is not installed\n";
            return 1;
        }
        std::cout << "would remove " << found->second.directory << "\n";
        return 0;
    }

    const kap::registry::InstallResult result =
        kap::registry::remove(args[0], lock, search_root(global), global.verbose);
    if (!result.installed) {
        std::cerr << "kap: error: " << result.message << "\n";
        return 1;
    }
    std::cout << "removed " << result.name
              << (result.origin == kap::registry::Origin::Link
                      ? " (the linked working copy was left alone)"
                      : "")
              << "\n";
    return 0;
}

// `kap plugin update [name]` — §6.1. With no name, updates everything.
int run_plugin_update(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (args.size() > 1) {
        std::cerr << "kap: usage: kap plugin update [name]\n";
        return 2;
    }
    const std::filesystem::path root = search_root(global);
    kap::registry::Lockfile     lock = open_lockfile();

    std::optional<kap::registry::Index> index;
    try {
        index = kap::registry::load_index(root);
    }
    catch (const kap::diag::Error& error) {
        std::cerr << error.report();
        return 1;
    }

    std::vector<std::string> names;
    if (args.size() == 1) {
        names.push_back(args[0]);
    } else {
        for (const auto& [name, unused] : lock.plugins) {
            (void) unused;
            names.push_back(name);
        }
    }
    if (names.empty()) {
        std::cout << "nothing to update\n";
        return 0;
    }

    int failures = 0;
    for (const std::string& name : names) {
        if (global.dry_run) {
            std::cout << "would update " << name << "\n";
            continue;
        }
        const kap::registry::InstallResult result =
            kap::registry::update(name, lock, index, root, global.verbose);
        if (result.installed)
            std::cout << "updated " << result.name << " to " << result.version << "\n";
        else if (args.size() == 1) {
            // An explicit `kap plugin update <name>` that did nothing is a
            // failure; the same message during "update everything" is just
            // information about one plugin that was pinned or linked.
            std::cerr << "kap: error: " << result.message << "\n";
            ++failures;
        } else {
            std::cout << "skipped " << name << ": " << result.message << "\n";
        }
    }
    return failures == 0 ? 0 : 1;
}

// `kap plugin enable|disable <name>` — §6.1. Recorded in the lockfile rather
// than by moving files, so the change is reversible and the plugin stays
// inspectable while it is off.
int run_plugin_toggle(const kap::cli::GlobalOptions&  global,
                      const std::vector<std::string>& args,
                      bool                            enable)
{
    const char* verb = enable ? "enable" : "disable";
    if (args.size() != 1) {
        std::cerr << "kap: usage: kap plugin " << verb << " <name>\n";
        return 2;
    }
    const std::filesystem::path root = search_root(global);
    kap::registry::Lockfile     lock = open_lockfile();

    auto found = lock.plugins.find(args[0]);
    if (found == lock.plugins.end()) {
        // A plugin discovered on disk but absent from the lockfile is normal:
        // a bundled or in-repo plugin was never "installed". Toggling it still
        // has to work, so a row is created for it.
        const std::vector<kap::plugin::Located> discovered = discover_with_lockfile(root, lock);
        const auto                              on_disk =
            std::find_if(discovered.begin(),
                         discovered.end(),
                         [&args](const kap::plugin::Located& p) { return p.name == args[0]; });
        if (on_disk == discovered.end()) {
            std::cerr << "kap: error: no plugin named '" << args[0] << "'\n";
            return 1;
        }
        kap::registry::Installed row;
        row.name      = args[0];
        row.origin    = kap::registry::Origin::Local;
        row.directory = on_disk->directory.string();
        found         = lock.plugins.emplace(args[0], std::move(row)).first;
    }

    if (found->second.enabled == enable) {
        std::cout << args[0] << " is already " << (enable ? "enabled" : "disabled") << "\n";
        return 0;
    }
    if (global.dry_run) {
        std::cout << "would " << verb << " " << args[0] << "\n";
        return 0;
    }

    found->second.enabled = enable;
    kap::registry::save_lockfile(lock);
    kap::detect::invalidate_cache(root); // the candidate set changed
    std::cout << (enable ? "enabled " : "disabled ") << args[0] << "\n";
    return 0;
}

// `kap plugin pin <name> <version>` — §6.1, plus `--clear` to undo it.
int run_plugin_pin(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    std::vector<std::string> positional;
    bool                     clear = false;
    for (const std::string& arg : args) {
        if (arg == "--clear")
            clear = true;
        else if (arg.size() > 1 && arg[0] == '-') {
            std::cerr << "kap: error: unknown option '" << arg << "' for 'kap plugin pin'\n";
            return 2;
        } else
            positional.push_back(arg);
    }

    if ((clear && positional.size() != 1) || (!clear && positional.size() != 2)) {
        std::cerr << "kap: usage: kap plugin pin <name> <version>\n"
                     "      note:  kap plugin pin <name> --clear\n";
        return 2;
    }

    kap::registry::Lockfile lock  = open_lockfile();
    const auto              found = lock.plugins.find(positional[0]);
    if (found == lock.plugins.end()) {
        std::cerr << "kap: error: '" << positional[0] << "' is not installed\n"
                  << "      note: only an installed plugin has a version to pin\n";
        return 1;
    }

    if (global.dry_run) {
        std::cout << "would " << (clear ? "unpin " : "pin ") << positional[0];
        if (!clear)
            std::cout << " to " << positional[1];
        std::cout << "\n";
        return 0;
    }

    found->second.pinned = clear ? std::string() : positional[1];
    kap::registry::save_lockfile(lock);
    if (clear)
        std::cout << "unpinned " << positional[0] << "\n";
    else
        std::cout << "pinned " << positional[0] << " to " << positional[1] << "\n";
    return 0;
}

// `kap plugin new <name> [--template build-system]` — §6.1.
int run_plugin_new(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    std::string              name;
    std::string              template_name = "build-system";
    std::vector<std::string> positional;

    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "--template") {
            if (index + 1 >= args.size()) {
                std::cerr << "kap: error: --template requires a name\n";
                return 2;
            }
            template_name = args[++index];
        } else if (arg.starts_with("--template=")) {
            template_name = arg.substr(std::string("--template=").size());
        } else if (arg.size() > 1 && arg[0] == '-') {
            std::cerr << "kap: error: unknown option '" << arg << "' for 'kap plugin new'\n";
            return 2;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() != 1) {
        std::string available;
        for (const std::string& option : kap::registry::templates())
            available += (available.empty() ? "" : ", ") + option;
        std::cerr << "kap: usage: kap plugin new <name> [--template " << available << "]\n";
        return 2;
    }
    name = positional[0];

    const std::filesystem::path directory = search_root(global) / name;
    if (global.dry_run) {
        std::cout << "would scaffold '" << name << "' (" << template_name << ") into "
                  << directory.string() << "\n";
        return 0;
    }

    const kap::registry::InstallResult result =
        kap::registry::scaffold(name, template_name, directory);
    if (!result.installed) {
        std::cerr << "kap: error: " << result.message << "\n";
        return 1;
    }

    std::cout
        << "created " << result.directory.string() << "\n"
        << "  plugin.kpl                                    the whole plugin\n"
        << "  README.md                                     what it does and how to configure it\n"
        << "  tests/fixtures/example/                        a fake project to evaluate against\n"
        << "  tests/expected/example.build.steps.json        the commands that should produce\n"
        << "\nnext:\n"
        << "  kap plugin doctor --root .          parse, validate, and type-check it\n"
        << "  kap plugin test " << name << "\n"
        << "  kap plugin install --link " << directory.string() << "\n";
    return 0;
}

// Resolve the arguments `kap plugin doctor` and `kap plugin test` accept.
//
// Both take zero or more of: a plugin *name* (matched against what discovery
// found) or a *path* to a plugin directory. The path form is what a plugin
// author reaches for first — `kap plugin new my-thing` then
// `kap plugin doctor my-thing` — and without it they would have to install the
// thing before they could check whether it parses, which is backwards.
//
// Returns false after printing the complaint when a name or path resolves to
// nothing.
bool resolve_plugin_arguments(const std::filesystem::path&       root,
                              const std::vector<std::string>&    requested,
                              std::vector<kap::plugin::Located>& out)
{
    const kap::registry::Lockfile           lock       = open_lockfile();
    const std::vector<kap::plugin::Located> discovered = discover_with_lockfile(root, lock);

    if (requested.empty()) {
        out = discovered;
        if (out.empty()) {
            std::cerr << "kap: error: no plugins found\n"
                         "      note: searched "
                      << (root / ".kap" / "plugins").string()
                      << ",\n"
                         "      note: $KAP_PLUGIN_PATH, "
                      << (kap::paths::user_plugin_dir().empty()
                              ? std::string("~/.local/share/kap/plugins")
                              : kap::paths::user_plugin_dir().string())
                      << ",\n      note: and " << (root / "plugins").string()
                      << "\n"
                         "      note: pass a directory to check one directly: "
                         "kap plugin doctor ./my-plugin\n";
            return false;
        }
        return true;
    }

    for (const std::string& argument : requested) {
        const auto by_name =
            std::find_if(discovered.begin(),
                         discovered.end(),
                         [&argument](const kap::plugin::Located& p) { return p.name == argument; });
        if (by_name != discovered.end()) {
            out.push_back(*by_name);
            continue;
        }

        // Not a known name — try it as a directory containing a plugin.kpl.
        std::filesystem::path directory(argument);
        if (directory.is_relative())
            directory = root / directory;
        if (kap::fs::is_file(directory / "plugin.kpl")) {
            kap::plugin::Located located;
            // filename() of a path ending in "/" is empty, which would produce
            // a nameless plugin in every message; lexically_normal drops the
            // trailing separator first.
            located.name      = directory.lexically_normal().filename().string();
            located.directory = directory;
            located.manifest  = directory / "plugin.kpl";
            located.source    = kap::plugin::Source::ProjectLocal;
            out.push_back(std::move(located));
            continue;
        }

        std::cerr << "kap: error: no plugin named '" << argument << "', and "
                  << (directory / "plugin.kpl").string() << " does not exist\n";
        if (!discovered.empty()) {
            std::cerr << "      note: installed:";
            for (const kap::plugin::Located& located : discovered)
                std::cerr << ' ' << located.name;
            std::cerr << "\n";
        }
        return false;
    }
    return true;
}

// `kap plugin doctor [name|path]...` — design doc §6.1.
//
// The gate that keeps a plugin which cannot possibly run from being reported
// as healthy, so it checks everything: the file parses, the manifest has what
// §6.3 step 3 requires, api_version is one this kap supports, the detect rules
// are well-formed, and every declared command type-checks. A plugin whose
// manifest is fine but whose `build` command references an undeclared config
// key is broken, and saying "[PASS]" about it would be a lie.
int run_plugin_doctor(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    const std::filesystem::path       root = search_root(global);
    std::vector<kap::plugin::Located> plugins;
    if (!resolve_plugin_arguments(root, args, plugins))
        return 1;

    int failures = 0;
    for (const kap::plugin::Located& located : plugins) {
        const std::vector<std::string> problems =
            kap::registry::validate_payload(located.directory);
        if (problems.empty()) {
            std::cout << "[PASS] " << located.name << "\n";
            if (global.verbose)
                std::cout << "       " << located.manifest.string() << "\n";
            continue;
        }
        ++failures;
        std::cout << "[FAIL] " << located.name << "\n";
        std::cerr << "kap: error: " << located.manifest.string() << ":\n";
        for (const std::string& problem : problems)
            std::cerr << "  " << problem << "\n";
    }
    return failures == 0 ? 0 : 1;
}

// `kap plugin test [name|path]...` — design doc §6.1, Milestone 3's exit
// criterion. No build tool is ever executed: a case evaluates a command block
// against a fixture directory and compares the resulting CommandSpec with a
// committed golden file.
int run_plugin_test(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    const std::filesystem::path       root = search_root(global);
    std::vector<kap::plugin::Located> plugins;
    if (!resolve_plugin_arguments(root, args, plugins))
        return 1;

    // Load through the KPL AST cache (design doc §5.14). It is transparent —
    // a cache hit and a fresh parse produce the same AST — so this is purely
    // about not re-lexing a plugin that has not changed.
    const std::filesystem::path cache = kap::kapc::cache_directory();
    if (global.verbose)
        std::cerr << "kap: AST cache: " << (cache.empty() ? "disabled" : cache.string()) << "\n";

    int passed        = 0;
    int failed        = 0;
    int without_cases = 0;

    for (const kap::plugin::Located& located : plugins) {
        const std::vector<kap::plugin::CaseResult> results = kap::plugin::run_tests(located, cache);
        if (results.empty()) {
            ++without_cases;
            std::cout << "[SKIP] " << located.name << " (no test cases)\n";
            continue;
        }
        for (const kap::plugin::CaseResult& result : results) {
            if (result.passed) {
                ++passed;
                std::cout << "[PASS] " << located.name << " " << result.name << "\n";
                continue;
            }
            ++failed;
            std::cout << "[FAIL] " << located.name << " " << result.name << "\n";
            std::cerr << "      " << result.detail << "\n";
        }
    }

    std::cout << passed + failed << " cases: " << passed << " passed, " << failed << " failed";
    if (without_cases != 0)
        std::cout << ", " << without_cases << " plugin(s) without cases";
    std::cout << "\n";
    return failed == 0 ? 0 : 1;
}

// `kap plugin <subcommand> ...` — design doc §6.1. As with `kap config`,
// an unknown subcommand is refused rather than silently treated as one of the
// implemented ones.
int run_plugin(const kap::cli::GlobalOptions& global, const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        std::cerr
            << "usage: kap plugin <subcommand> [options]\n"
               "\n"
               "  list                       show what is installed\n"
               "  search <query>             search the registry index\n"
               "  install <name|url|path>    install from the registry, a git URL, or a path\n"
               "  install --bundle <name>    install a curated set\n"
               "  install --link <path>      symlink a working copy for development\n"
               "  remove <name>              uninstall\n"
               "  update [name]              update one plugin, or all of them\n"
               "  enable|disable <name>      toggle without uninstalling\n"
               "  pin <name> <version>       lock a version (--clear to unpin)\n"
               "  new <name>                 scaffold a new plugin\n"
               "  test [name]                run a plugin's fixture cases\n"
               "  doctor                     parse, validate, and type-check every plugin\n";
        return 2;
    }
    const std::string&             subcommand = argv[0];
    const std::vector<std::string> rest(argv.begin() + 1, argv.end());

    if (subcommand == "doctor")
        return run_plugin_doctor(global, rest);
    if (subcommand == "test")
        return run_plugin_test(global, rest);
    if (subcommand == "list")
        return run_plugin_list(global, rest);
    if (subcommand == "search")
        return run_plugin_search(global, rest);
    if (subcommand == "install")
        return run_plugin_install(global, rest);
    if (subcommand == "remove" || subcommand == "uninstall")
        return run_plugin_remove(global, rest);
    if (subcommand == "update")
        return run_plugin_update(global, rest);
    if (subcommand == "enable")
        return run_plugin_toggle(global, rest, true);
    if (subcommand == "disable")
        return run_plugin_toggle(global, rest, false);
    if (subcommand == "pin")
        return run_plugin_pin(global, rest);
    if (subcommand == "new")
        return run_plugin_new(global, rest);

    std::cerr << "kap: error: unknown subcommand 'plugin " << subcommand << "'\n"
              << "      note: expected one of: list, search, install, remove, update,\n"
              << "      note:                  enable, disable, pin, new, test, doctor\n";
    return 2;
}

// The search root: --root wins, otherwise the current directory (design doc
// §3.2 step 1).
std::filesystem::path search_root(const kap::cli::GlobalOptions& global)
{
    return global.root.value_or(std::filesystem::current_path());
}

// Render one TOML value as text for `kap config get`.
void print_value(const kap::toml::Value& value)
{
    using Kind = kap::toml::Value::Kind;
    switch (value.kind) {
        case Kind::String:
            std::cout << value.str << "\n";
            break;
        case Kind::Integer:
            std::cout << value.integer << "\n";
            break;
        case Kind::Boolean:
            std::cout << (value.boolean ? "true" : "false") << "\n";
            break;
        case Kind::Array:
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) {
                    std::cout << " ";
                }
                const kap::toml::Value& element = value.array[i];
                switch (element.kind) {
                    case Kind::String:
                        std::cout << element.str;
                        break;
                    case Kind::Integer:
                        std::cout << element.integer;
                        break;
                    case Kind::Boolean:
                        std::cout << (element.boolean ? "true" : "false");
                        break;
                    default:
                        std::cout << "<nested>";
                        break;
                }
            }
            std::cout << "\n";
            break;
        case Kind::Table:
            std::cout << "{ table }\n";
            break;
    }
}

// Which configuration file a `kap config` subcommand should act on.
//
// `get` defaults to the *merged* view, because "what will kap actually do" is
// the question a user has. `set` and `edit` need one concrete file, and default
// to the project's kap.toml — the one that belongs in the repository.
enum class ConfigTarget
{
    Merged,
    Global,
    Project,
};

// Parse the shared `--global` / `--project` selector out of a subcommand's
// arguments, leaving the positionals behind. Returns false on an unknown flag,
// having already printed the complaint.
bool take_config_target(std::vector<std::string>& args, ConfigTarget& target)
{
    std::vector<std::string> positional;
    for (const std::string& arg : args) {
        if (arg == "--global") {
            target = ConfigTarget::Global;
        } else if (arg == "--project") {
            target = ConfigTarget::Project;
        } else if (arg.size() > 1 && arg[0] == '-') {
            std::cerr << "kap: error: unknown option '" << arg << "' for 'kap config'\n"
                      << "      note: expected --global or --project\n";
            return false;
        } else {
            positional.push_back(arg);
        }
    }
    args = std::move(positional);
    return true;
}

std::filesystem::path config_file_for(ConfigTarget target, const std::filesystem::path& root)
{
    return target == ConfigTarget::Global ? kap::config::global_file()
                                          : kap::config::project_file(root);
}

// `kap config get <key>` (design doc §8).
//
// Reads the *effective* configuration — schema-independent kap settings and
// every `[plugins.<name>]` table, merged global-then-project (§5.12) — so the
// answer is what kap will act on rather than what one file happens to say.
// `--global` and `--project` narrow it to a single file when that is the
// question.
int run_config_get(const kap::cli::GlobalOptions& global, std::vector<std::string> args)
{
    ConfigTarget target = ConfigTarget::Merged;
    if (!take_config_target(args, target))
        return 2;
    if (args.size() != 1) {
        std::cerr << "kap: usage: kap config get [--global|--project] <key>\n";
        return 2;
    }
    const std::string&          key  = args[0];
    const std::filesystem::path root = search_root(global);

    kap::toml::Value table = kap::toml::make_table();
    std::string      where;

    if (target == ConfigTarget::Merged) {
        const kap::config::Merged merged = kap::config::load(root);
        if (merged.layers.empty()) {
            std::cerr << "kap: error: no configuration file found\n"
                      << "      note: looked for " << kap::config::global_file().string() << "\n"
                      << "      note:        and " << kap::config::project_file(root).string()
                      << "\n";
            return 1;
        }
        table = kap::config::effective(merged);
        where = "the merged configuration";
        if (global.verbose) {
            for (const kap::config::Layer& layer : merged.layers)
                std::cerr << "kap: reading " << layer.file.string() << "\n";
        }
    } else {
        const std::filesystem::path file = config_file_for(target, root);
        if (file.empty() || !kap::fs::is_file(file)) {
            std::cerr << "kap: error: no configuration file at "
                      << (file.empty() ? std::string("(unknown location)") : file.string()) << "\n";
            return 1;
        }
        if (global.verbose)
            std::cerr << "kap: reading " << file.string() << "\n";
        table = kap::toml::parse(kap::fs::read_text(file), file.string()).root();
        where = file.string();
    }

    kap::toml::Document document;
    document.root() = std::move(table);

    const auto value = document.get(key);
    if (!value) {
        std::cerr << "kap: error: no key '" << key << "' in " << where << "\n";
        return 1;
    }
    print_value(*value);
    return 0;
}

// `kap config set <key> <value>` (design doc §8). Writes ./kap.toml by
// default, ~/.config/kap/config.toml with --global.
int run_config_set(const kap::cli::GlobalOptions& global, std::vector<std::string> args)
{
    ConfigTarget target = ConfigTarget::Project;
    if (!take_config_target(args, target))
        return 2;
    if (target == ConfigTarget::Merged)
        target = ConfigTarget::Project;
    if (args.size() != 2) {
        std::cerr << "kap: usage: kap config set [--global|--project] <key> <value>\n"
                     "      note: example: kap config set plugins.cmake-cpp.generator ninja\n";
        return 2;
    }

    const std::filesystem::path file = config_file_for(target, search_root(global));
    if (file.empty()) {
        std::cerr << "kap: error: cannot locate the global configuration file\n"
                     "      note: neither $XDG_CONFIG_HOME nor $HOME is set\n";
        return 1;
    }

    if (global.dry_run) {
        std::cout << "would set " << args[0] << " = " << args[1] << " in " << file.string() << "\n";
        return 0;
    }

    try {
        kap::config::set_key(file, args[0], args[1]);
    }
    catch (const kap::diag::Error& error) {
        std::cerr << error.report();
        return 1;
    }
    std::cout << "set " << args[0] << " in " << file.string() << "\n";
    return 0;
}

// `kap config edit` (design doc §8): open a configuration file in $EDITOR.
//
// The file is created empty first if it does not exist, because most editors
// handle "open this new file" gracefully but `kap config edit` on a machine
// with no config should still land you somewhere you can type.
int run_config_edit(const kap::cli::GlobalOptions& global, std::vector<std::string> args)
{
    ConfigTarget target = ConfigTarget::Project;
    if (!take_config_target(args, target))
        return 2;
    if (target == ConfigTarget::Merged)
        target = ConfigTarget::Project;
    if (!args.empty()) {
        std::cerr << "kap: usage: kap config edit [--global|--project]\n";
        return 2;
    }

    const std::filesystem::path file = config_file_for(target, search_root(global));
    if (file.empty()) {
        std::cerr << "kap: error: cannot locate the global configuration file\n";
        return 1;
    }

    std::string editor = kap::paths::env_or_empty("VISUAL");
    if (editor.empty())
        editor = kap::paths::env_or_empty("EDITOR");
    if (editor.empty()) {
        // No guessing at vi/nano: launching an editor the user did not ask for
        // is a genuinely hostile surprise in a terminal, and the fix is one
        // line they should know about anyway.
        std::cerr << "kap: error: no editor configured\n"
                     "      note: set $EDITOR or $VISUAL, or edit the file directly:\n"
                     "      note:     "
                  << file.string() << "\n";
        return 1;
    }

    if (!kap::fs::exists(file)) {
        std::error_code ec;
        if (!file.parent_path().empty())
            std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream created(file, std::ios::binary | std::ios::app);
    }

    kap::kpl::CommandSpec spec;
    kap::kpl::Step        step;
    // The editor name may itself carry arguments ("code --wait"), which is
    // conventional for $EDITOR; splitting on spaces is what every other tool
    // that honours the variable does.
    std::istringstream words(editor);
    std::string        word;
    while (words >> word)
        step.command.push_back(word);
    step.command.push_back(file.string());
    spec.steps.push_back(std::move(step));

    kap::exec::Options options;
    options.root    = search_root(global);
    options.dry_run = global.dry_run;
    options.verbose = global.verbose;
    options.color   = kap::exec::default_color();
    return kap::exec::run(spec, options).exit_code;
}

// `kap config <subcommand> ...` — design doc §8 lists get/set/edit.
//
// Dispatching on the subcommand matters more than it looks: the first version
// ran a `get` for ANY subcommand, so `kap config set a b` quietly performed a
// lookup and `kap config nonsense server.host` printed a value and exited 0.
// A CLI that silently does something other than what was typed is worse than
// one that refuses.
int run_config(const kap::cli::GlobalOptions& global, const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        std::cerr << "kap: usage: kap config <get|set|edit> ...\n";
        return 2;
    }

    const std::string        subcommand = argv[0];
    std::vector<std::string> rest(argv.begin() + 1, argv.end());

    if (subcommand == "get")
        return run_config_get(global, std::move(rest));
    if (subcommand == "set")
        return run_config_set(global, std::move(rest));
    if (subcommand == "edit")
        return run_config_edit(global, std::move(rest));

    std::cerr << "kap: error: unknown subcommand 'config " << subcommand << "'\n"
              << "      note: expected one of: get, set, edit\n";
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        // argv[0] is the program name; everything after is what we parse.
        const std::vector<std::string> args(argv + 1, argv + argc);
        const kap::cli::Invocation     inv = kap::cli::parse(args);

        if (inv.global.version) {
            std::cout << kap::kProgramName << " " << kap::kVersionString << "\n";
            return 0;
        }
        if (inv.global.help) {
            print_usage(std::cout);
            return 0;
        }
        if (inv.command.empty()) {
            // Bare `kap`: print version, exit 0 (the Milestone-0 contract).
            std::cout << kap::kProgramName << " " << kap::kVersionString << "\n";
            return 0;
        }

        if (inv.command == "config") {
            return run_config(inv.global, inv.argv);
        }
        if (inv.command == "detect") {
            return run_detect(inv.global, inv.argv);
        }
        if (inv.command == "plugin") {
            return run_plugin(inv.global, inv.argv);
        }
        if (is_project_command(inv.command)) {
            return run_project_command(inv.command, inv);
        }

        // Milestone 6's "missing command -> clear error". A user who typed
        // `kap buidl` should be told what kap *does* know, not handed a usage
        // banner and left to spot the difference.
        std::cerr << "kap: error: unknown command '" << inv.command << "'\n"
                  << "      note: project commands:";
        for (const std::string& name : project_commands())
            std::cerr << ' ' << name;
        std::cerr << "\n      note: kap commands: config detect plugin\n";
        return 2;
    }
    catch (const kap::diag::Error& e) {
        // Every subsystem reports failures through diag::Error; one catch
        // here converts any of them into the same styled stderr line.
        std::cerr << e.report();
        return 1;
    }
    catch (const std::exception& e) {
        // Not everything that can go wrong is a diag::Error. std::filesystem
        // throws filesystem_error (current_path() fails if the working
        // directory was deleted out from under us), std::bad_alloc exists, and
        // future subsystems will have their own. Without this clause any of
        // them would escape main and abort the process, printing a bare
        // "terminate called after throwing..." with no context. Catching them
        // costs nothing and keeps every failure a normal exit code.
        std::cerr << "kap: error: " << e.what() << "\n";
        return 1;
    }
}