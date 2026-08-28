#pragma once

// core/cli.hpp
//
// The subcommand parser (design doc Milestone 1). It understands kap's global
// flags, the first positional token as the command, everything before `--` as
// the command's own arguments, and everything after `--` as *passthrough* —
// arguments for the real tool a plugin will invoke (design doc §4:
// `kap build -- --release`).
//
// Global flags may appear anywhere before `--`:
//   --dry-run / -n        don't run, just print what would run
//   --verbose             extra logging
//   --root <path>         search root (design doc §3.2)
//   --set key=value       override one config key (design doc §5.12)
//   --help / -h, --version / -V
//
// Failure to parse (unknown option, missing value) throws diag::Error, so the
// caller only needs one catch at the top of main.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kap
{
namespace cli
{

// Flags that apply to every kap invocation, independent of the command.
struct GlobalOptions
{
    bool dry_run = false;
    bool verbose = false;
    bool help    = false;
    bool version = false;

    // --root <path>: where to start project detection (§3.2). Empty when the
    // user did not override — the engine defaults to the current directory.
    std::optional<std::filesystem::path> root;

    // --set key=value, repeatable; the highest-precedence config override.
    std::vector<std::string> set_values;
};

// A fully parsed command line.
struct Invocation
{
    std::string              command;     // "build", "plugin", "config", ... ("" if none)
    std::vector<std::string> argv;        // the command's own arguments
    std::vector<std::string> passthrough; // arguments after `--`, for the wrapped tool

    GlobalOptions global;
};

// Parse a raw argv list (any tokenization the OS gives us). Throws
// diag::Error on malformed input; never returns a half-parsed invocation.
Invocation parse(const std::vector<std::string>& args);

} // namespace cli
} // namespace kap