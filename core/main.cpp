// core/main.cpp
//
// kap — "know project, act". A zero-config CLI that detects what kind of
// project you're standing in and runs the right underlying tool for common
// tasks (build, test, lint, run, ...). See docs/design.md for the full design.
//
// Milestones 0-3 are wired: the CLI parser (core/cli.hpp), the TOML parser
// (core/toml.hpp), and the whole KPL front-end, type checker, and interpreter
// (core/kpl.hpp) behind `kap plugin doctor` and `kap plugin test`.
//
// The detection engine, the executor, and the config merge arrive in
// Milestones 4-6. Until then there is deliberately no `kap build`: it would
// have to guess which plugin applies, and guessing is the one thing design doc
// §3.2 step 4 forbids.

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "core/cli.hpp"
#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/kapc.hpp"
#include "core/kpl.hpp"
#include "core/plugin.hpp"
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
           "commands:\n"
           "  config get <key>     read one key from ./kap.toml (or --root)\n"
           "  plugin doctor        validate bundled plugin files\n"
           "  plugin test [name]   run plugin fixture tests\n"
           "\n"
           "global flags:\n"
           "  -n, --dry-run      print actions without running them\n"
           "      --root <path>  search root for the project\n"
           "      --set k=v      override one config key (repeatable)\n"
           "      --verbose      extra logging\n"
           "\n"
           "kap is under construction (Milestone 3). See docs/design.md for\n"
           "the roadmap.\n";
}

std::filesystem::path search_root(const kap::cli::GlobalOptions& global);

// `kap plugin doctor` — parse, manifest-validate, and type-check every bundled
// plugin (design doc §6.1). This is the gate that keeps a plugin which cannot
// possibly run from being reported as healthy, so it checks all three: a
// plugin whose manifest is fine but whose `build` command references an
// undeclared config key is broken, and saying "[PASS]" about it would be a lie.
int run_plugin_doctor(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (!args.empty()) {
        std::cerr << "kap: usage: kap plugin doctor\n";
        return 2;
    }
    const std::filesystem::path             root     = search_root(global);
    const std::vector<kap::plugin::Located> plugins  = kap::plugin::discover(root);
    int                                     failures = 0;

    for (const kap::plugin::Located& located : plugins) {
        try {
            const kap::kpl::Plugin plugin =
                kap::kpl::parse(kap::fs::read_text(located.manifest), located.manifest.string());

            std::vector<std::string>       errors      = kap::kpl::validate(plugin);
            const std::vector<std::string> type_errors = kap::kpl::type_check(plugin);
            errors.insert(errors.end(), type_errors.begin(), type_errors.end());

            if (errors.empty()) {
                std::cout << "[PASS] " << located.name << "\n";
                continue;
            }
            ++failures;
            std::cerr << "kap: error: " << located.manifest.string() << ":\n";
            for (const std::string& error : errors)
                std::cerr << "  " << error << "\n";
        }
        catch (const kap::diag::Error& error) {
            ++failures;
            std::cerr << error.report();
        }
    }

    if (plugins.empty()) {
        std::cerr << "kap: error: no plugin.kpl files found in " << (root / "plugins").string()
                  << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}

// `kap plugin test [name]` — run each plugin's fixture cases (design doc §6.1,
// Milestone 3 exit criterion). No build tool is ever executed: a case
// evaluates a command block against a fixture directory and compares the
// resulting CommandSpec with a committed golden file.
int run_plugin_test(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (args.size() > 1) {
        std::cerr << "kap: usage: kap plugin test [name]\n";
        return 2;
    }
    const std::filesystem::path       root  = search_root(global);
    std::vector<kap::plugin::Located> found = kap::plugin::discover(root);

    if (args.size() == 1) {
        const std::string& wanted = args[0];
        std::erase_if(found, [&wanted](const kap::plugin::Located& p) { return p.name != wanted; });
        if (found.empty()) {
            std::cerr << "kap: error: no plugin named '" << wanted << "' under "
                      << (root / "plugins").string() << "\n";
            return 1;
        }
    }
    if (found.empty()) {
        std::cerr << "kap: error: no plugin.kpl files found in " << (root / "plugins").string()
                  << "\n";
        return 1;
    }

    // Load through the KPL AST cache (design doc §5.14). It is transparent —
    // a cache hit and a fresh parse produce the same AST — so this is purely
    // about not re-lexing a plugin that has not changed.
    const std::filesystem::path cache = kap::kapc::cache_directory();
    if (global.verbose)
        std::cerr << "kap: AST cache: " << (cache.empty() ? "disabled" : cache.string()) << "\n";

    int passed        = 0;
    int failed        = 0;
    int without_cases = 0;

    for (const kap::plugin::Located& located : found) {
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
        std::cerr << "kap: usage: kap plugin <doctor|test> ...\n";
        return 2;
    }
    const std::string&             subcommand = argv[0];
    const std::vector<std::string> rest(argv.begin() + 1, argv.end());

    if (subcommand == "doctor")
        return run_plugin_doctor(global, rest);
    if (subcommand == "test")
        return run_plugin_test(global, rest);

    std::cerr << "kap: error: unknown subcommand 'plugin " << subcommand << "'\n"
              << "      note: expected one of: doctor, test\n";
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

// `kap config get <key>`: load <root>/kap.toml and print one dotted key.
// This is the Milestone-1 exit criterion ("config get reads a fixture TOML")
// and the seed of the full `kap config` command (Milestone 6).
//
// `args` is everything after the word "get".
int run_config_get(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (args.size() != 1) {
        std::cerr << "kap: usage: kap config get <key>\n";
        return 2;
    }
    const std::string& key = args[0];

    const std::filesystem::path root = search_root(global);
    const std::filesystem::path file = root / "kap.toml";

    if (global.verbose) {
        std::cerr << "kap: reading " << file.string() << "\n";
    }

    if (!kap::fs::exists(file)) {
        std::cerr << "kap: error: no configuration file at " << file.string() << "\n";
        return 1;
    }

    const std::string         text = kap::fs::read_text(file);
    const kap::toml::Document doc  = kap::toml::parse(text, file.string());

    const auto value = doc.get(key);
    if (!value) {
        std::cerr << "kap: error: no key '" << key << "' in " << file.string() << "\n";
        return 1;
    }

    print_value(*value);
    return 0;
}

// `kap config <subcommand> ...` — design doc §8 lists get/set/edit.
//
// Dispatching on the subcommand matters more than it looks: the first version
// ran a `get` for ANY subcommand, so `kap config set a b` quietly performed a
// lookup and `kap config nonsense server.host` printed a value and exited 0.
// A CLI that silently does something other than what was typed is worse than
// one that refuses, so unknown subcommands are now an error, and the two that
// are designed but not yet built say exactly that.
int run_config(const kap::cli::GlobalOptions& global, const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        std::cerr << "kap: usage: kap config get <key>\n";
        return 2;
    }

    const std::string&             subcommand = argv[0];
    const std::vector<std::string> rest(argv.begin() + 1, argv.end());

    if (subcommand == "get") {
        return run_config_get(global, rest);
    }
    if (subcommand == "set" || subcommand == "edit") {
        // Declared in design doc §8, scheduled for Milestone 6 (config merge).
        // Saying so beats a generic "unknown subcommand".
        std::cerr << "kap: error: 'kap config " << subcommand
                  << "' is not implemented yet (design doc Milestone 6)\n";
        return 2;
    }

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
        if (inv.command == "plugin") {
            return run_plugin(inv.global, inv.argv);
        }

        std::cerr << "kap: unknown command '" << inv.command << "'\n";
        print_usage(std::cerr);
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