// core/main.cpp
//
// kap — "know project, act". A zero-config CLI that detects what kind of
// project you're standing in and runs the right underlying tool for common
// tasks (build, test, lint, run, ...). See docs/design.md for the full design.
//
// Milestone 1: the CLI parser (core/cli.hpp) and TOML parser (core/toml.hpp)
// are now wired. The detection engine + KPL interpreter arrive in later
// milestones; today `kap` only knows how to print its version/help and read a
// project config file via `kap config get`.

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "core/cli.hpp"
#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/kpl.hpp"
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
           "  config get <key>   read one key from ./kap.toml (or --root)\n"
           "  plugin doctor      validate bundled plugin files\n"
           "\n"
           "global flags:\n"
           "  -n, --dry-run      print actions without running them\n"
           "      --root <path>  search root for the project\n"
           "      --set k=v      override one config key (repeatable)\n"
           "      --verbose      extra logging\n"
           "\n"
           "kap is under construction (Milestone 1). See docs/design.md for\n"
           "the roadmap.\n";
}

std::filesystem::path search_root(const kap::cli::GlobalOptions& global);

int run_plugin_doctor(const kap::cli::GlobalOptions& global, const std::vector<std::string>& args)
{
    if (!args.empty()) {
        std::cerr << "kap: usage: kap plugin doctor\n";
        return 2;
    }
    const std::filesystem::path    plugin_root = search_root(global) / "plugins";
    const std::vector<std::string> plugin_dirs = kap::fs::glob(plugin_root, "*");
    int                            failures    = 0;
    int                            checked     = 0;
    for (const std::string& plugin_dir : plugin_dirs) {
        const std::filesystem::path source = plugin_root / plugin_dir / "plugin.kpl";
        if (!kap::fs::is_file(source))
            continue;
        ++checked;
        try {
            const kap::kpl::Plugin plugin =
                kap::kpl::parse(kap::fs::read_text(source), source.string());
            const std::vector<std::string> errors = kap::kpl::validate(plugin);
            if (errors.empty()) {
                std::cout << "[PASS] " << plugin_dir << "\n";
                continue;
            }
            ++failures;
            std::cerr << "kap: error: " << source.string() << ":\n";
            for (const std::string& error : errors)
                std::cerr << "  " << error << "\n";
        }
        catch (const kap::diag::Error& error) {
            ++failures;
            std::cerr << error.report();
        }
    }
    if (checked == 0) {
        std::cerr << "kap: error: no plugin.kpl files found in " << plugin_root.string() << "\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
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
        if (inv.command == "plugin" && inv.argv.size() >= 1 && inv.argv[0] == "doctor") {
            return run_plugin_doctor(
                inv.global, std::vector<std::string>(inv.argv.begin() + 1, inv.argv.end()));
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