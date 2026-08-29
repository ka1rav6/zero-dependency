#pragma once

// core/completions.hpp
//
// Shell completion scripts (design doc Milestone 10).
//
// The scripts are generated rather than checked in, for one reason: the command
// lists they complete live in core/main.cpp, and a checked-in script is a
// second copy that drifts the first time somebody adds a subcommand. `kap
// completions <shell>` prints the script; the lists it embeds come from the
// same functions the CLI dispatches on, so the two cannot disagree.
//
// Static completion only — the scripts do not shell out to kap to ask what
// plugins are installed. A completion script that runs a program on every Tab
// makes the shell feel broken the first time that program is slow, and kap's
// dynamic answers (plugin names, config keys) all depend on the current
// directory in ways a cached script would get wrong anyway.

#include <string>
#include <string_view>
#include <vector>

namespace kap
{
namespace completions
{

// The shells with a script. Named here so `kap completions` can list them.
std::vector<std::string> shells();

// The completion script for `shell`, or an empty string when it is not one of
// `shells()`. `project_commands` and `plugin_subcommands` are passed in rather
// than duplicated here, so the scripts complete exactly what the CLI accepts.
std::string script(std::string_view                shell,
                   const std::vector<std::string>& project_commands,
                   const std::vector<std::string>& plugin_subcommands);

} // namespace completions
} // namespace kap
