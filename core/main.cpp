// core/main.cpp
//
// kap — "know project, act". A zero-config CLI that detects what kind of
// project you're standing in and runs the right underlying tool for common
// tasks (build, test, lint, run, ...). See docs/design.md for the full design.
//
// Milestone 0 (design doc §11): an *empty* binary that prints its version and
// exits 0. The real subcommand parser lands in Milestone 1 (core/cli.hpp);
// until then, --version / --help are the only supported invocations, and any
// real command fails loudly instead of silently doing nothing.

#include <iostream>
#include <string_view>

#include "core/version.hpp"

namespace
{

// Print the canonical usage banner. Kept inline here until Milestone 1
// replaces it with the real CLI parser.
void print_usage(std::ostream& out)
{
    out << "usage: kap <command> [options]\n"
           "       kap --version | --help\n"
           "\n"
           "kap is under construction (Milestone 0). See docs/design.md for\n"
           "the roadmap.\n";
}

} // namespace

int main(int argc, char** argv)
{
    // Walk argv just enough to answer --version / --help; full parsing is
    // Milestone 1.
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);

        if (arg == "--version" || arg == "-V") {
            std::cout << kap::kProgramName << " " << kap::kVersionString << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        }

        // A real subcommand (build, test, ...) exists only in later
        // milestones. Exit non-zero so scripts never mistake a no-op for
        // success.
        std::cerr << "kap: unknown argument '" << arg << "'\n";
        print_usage(std::cerr);
        return 2;
    }

    // No arguments: print version and exit 0 (the Milestone 0 contract).
    std::cout << kap::kProgramName << " " << kap::kVersionString << "\n";
    return 0;
}