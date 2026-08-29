#pragma once

// core/help.hpp
//
// Per-command help: `kap build --help`, `kap plugin install --help`,
// `kap detect -h`.
//
// ## Why this is not just a longer banner
//
// `-h` used to be a *global* flag, so `kap install -h` printed the same page as
// `kap -h` — the one place a person looks when they are already confused, and
// it told them nothing about the command they asked about. Worse, the command
// most likely to be asked about is `install`, whose behaviour surprises people.
//
// Each page answers four questions in the same order, because that is the order
// someone reads them in: what does this do, how do I spell it, what are the
// options, and what does a real invocation look like.
//
// The pages are data rather than code so that `kap help` can list every topic,
// the completion scripts could offer them, and adding a command means adding a
// row rather than finding the right `if`.

#include <string>
#include <string_view>
#include <vector>

namespace kap
{
namespace help
{

// One help topic: a command, or a `plugin`/`config` subcommand ("plugin
// install").
struct Topic
{
    std::string_view name;    // "build", "plugin install"
    std::string_view summary; // one line, used in the top-level command list
    std::string_view body;    // the full page, already formatted
};

// Every topic, in the order the top-level banner lists them.
const std::vector<Topic>& topics();

// The topic for `name`, or nullptr. `name` may be a command ("build") or a
// space-joined subcommand path ("plugin install").
const Topic* find(std::string_view name);

// The full page for `name`, or an empty string when there is no such topic.
std::string page(std::string_view name);

// True when `args` asks for help — `-h` or `--help` anywhere in them.
bool requested(const std::vector<std::string>& args);

// `args` with every `-h`/`--help` removed, so a command can strip the flag once
// and then parse what remains normally.
std::vector<std::string> without_help_flags(const std::vector<std::string>& args);

} // namespace help
} // namespace kap
