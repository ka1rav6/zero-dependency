// core/cli.cpp
//
// Implementation of the CLI parser declared in core/cli.hpp. Pure string
// logic — no args belong to the filesystem or environment — so it is trivial
// to unit test in isolation, which Milestone 1's exit criteria demand.

#include "core/cli.hpp"

#include <string>
#include <string_view>

#include "core/diag.hpp"

namespace kap
{
namespace cli
{

namespace
{

// A CLI mistake (unknown flag, missing value) is a located diagnostic pointing
// at "<argv>" — there is no source file for a command line.
[[noreturn]] void fail(const std::string& message)
{
    throw diag::Error{diag::error(message, diag::Location{"<argv>"})};
}

} // namespace

Invocation parse(const std::vector<std::string>& args)
{
    Invocation inv;
    bool       passthrough = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (passthrough) {
            // Everything after `--` belongs to the wrapped tool.
            inv.passthrough.push_back(arg);
            continue;
        }

        if (arg == "--") {
            passthrough = true;
            continue;
        }

        if (arg == "-n" || arg == "--dry-run") {
            inv.global.dry_run = true;
            continue;
        }
        if (arg == "--verbose") {
            inv.global.verbose = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            inv.global.help = true;
            continue;
        }
        if (arg == "-V" || arg == "--version") {
            inv.global.version = true;
            continue;
        }

        // --root accepts both "--root path" and "--root=path".
        if (arg == "--root" || arg.starts_with("--root=")) {
            std::string value;
            if (arg == "--root") {
                if (i + 1 >= args.size()) {
                    fail("--root requires a path argument");
                }
                value = args[++i];
            } else {
                value = arg.substr(std::string_view("--root=").size());
                if (value.empty()) {
                    fail("--root requires a path argument");
                }
            }
            inv.global.root = std::filesystem::path(value);
            continue;
        }

        // --set expects exactly one key=value pair (the highest-precedence
        // config override, design doc §5.12). Only the first '=' splits.
        if (arg == "--set" || arg.starts_with("--set=")) {
            std::string value;
            if (arg == "--set") {
                if (i + 1 >= args.size()) {
                    fail("--set requires a key=value argument");
                }
                value = args[++i];
            } else {
                value = arg.substr(std::string_view("--set=").size());
            }
            if (value.find('=') == std::string::npos) {
                fail("--set expects key=value, got '" + value + "'");
            }
            inv.global.set_values.push_back(std::move(value));
            continue;
        }

        // Any other dash-prefixed token is a flag kap does not know; fail
        // loudly rather than guessing. A lone "-" is conventionally stdin and
        // is kept as a positional.
        if (arg.size() > 1 && arg[0] == '-') {
            fail("unknown option '" + arg + "'");
        }

        // Positional tokens: the first one is the command, the rest are its
        // own arguments (design doc §8 CLI surface).
        if (inv.command.empty()) {
            inv.command = arg;
        } else {
            inv.argv.push_back(arg);
        }
    }

    return inv;
}

} // namespace cli
} // namespace kap