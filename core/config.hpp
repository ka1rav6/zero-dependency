#pragma once

// core/config.hpp
//
// The layered configuration (design doc §5.12, Milestone 6).
//
// §5.12's whole promise is that a user can change what a plugin does without
// forking it. Four layers, later winning:
//
//     schema defaults  (plugin.kpl's `schema` block, §5.7)
//   → global config    (~/.config/kap/config.toml)
//   → project config   (./kap.toml, committed alongside the code)
//   → --set key=value  (this invocation only)
//
// Two different things are configured, and they are kept apart on purpose:
//
//   * kap's own settings — `[detect] max_walk_up`, `[hooks] pre_build`, ...
//     These the core understands directly.
//   * per-plugin settings — `[plugins.<name>] generator = "ninja"`.
//     These the core does *not* understand: it only knows how to validate them
//     against the plugin's own `schema` block and hand them to the interpreter
//     as the `config` record. That is what keeps the binary free of ecosystem
//     knowledge (§3.1).
//
// Unknown keys under `[plugins.<name>]` are an error, not a warning (§5.7:
// "the core validates ... *before* invoking KPL, so typos fail fast with a
// clear error"). A silently ignored `genrator = "ninja"` would leave the user
// staring at a build that ignores their config for no visible reason.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/kpl.hpp"
#include "core/toml.hpp"

namespace kap
{
namespace config
{

// One configuration file that was found and parsed.
struct Layer
{
    std::string           name; // "global" / "project", for messages
    std::filesystem::path file;
    toml::Value           table = toml::make_table();
};

// kap's own settings, read out of the merged layers.
struct Settings
{
    // [detect] max_walk_up — how far up the tree detection may walk (§3.2
    // step 1). Default 0: this directory only, opt-in upward walk.
    int max_walk_up = 0;

    // [detect] ecosystem — the pin that settles a tie (§3.2 step 4).
    std::string ecosystem;

    // [hooks] pre_build = "...", post_test = "..." (§5.13), keyed by hook name.
    std::map<std::string, std::string> hooks;
};

// Everything one invocation needs to know about configuration.
struct Merged
{
    std::vector<Layer> layers; // in precedence order, lowest first
    Settings           settings;

    // Non-fatal complaints (an unreadable global config, an unknown key under
    // [detect]). Printed under --verbose, and on the error path where they may
    // be the explanation.
    std::vector<std::string> warnings;

    // The hook for `<phase>_<command>`, e.g. hook("pre", "build").
    std::optional<std::string> hook(const std::string& phase, const std::string& command) const;
};

// The global configuration file for this machine (~/.config/kap/config.toml,
// or $XDG_CONFIG_HOME/kap/config.toml). Empty when there is no $HOME.
std::filesystem::path global_file();

// The project configuration file for `root` (<root>/kap.toml).
std::filesystem::path project_file(const std::filesystem::path& root);

// Load and merge the global and project layers.
//
// A file that does not exist is simply absent — configuration is optional and
// kap must work in a bare directory. A file that exists but does not parse is
// a hard error: silently ignoring a config the user wrote would be worse than
// refusing to start.
Merged load(const std::filesystem::path& root);

// The merged view as a single TOML table, for `kap config get` with no
// `--global`/`--project` qualifier: exactly what kap will act on.
toml::Value effective(const Merged& merged);

// Build the `config` record for one plugin (§5.7 + §5.12).
//
// Precedence: the plugin's schema defaults, then `[plugins.<name>]` from each
// layer in order, then `set_values` (the repeatable `--set key=value`).
// `--set` strings are coerced to the schema field's declared type, so
// `--set release=true` produces a bool and `--set cmake_args=-DA=1,-DB=2` a
// list<str>.
//
// Errors are returned rather than thrown so every bad key can be reported at
// once instead of one per run.
struct PluginConfig
{
    std::map<std::string, kpl::Value> values;
    std::vector<std::string>          errors;
};

// `injected` are values the *core* supplies for this plugin — today only the
// bundled `doctor` plugin has any (design doc §4 and Milestone 9). They sit at
// the bottom of the chain, just above the schema defaults, so §5.12's "later
// wins" rule holds with no exception: both config files and `--set` override
// them, which is what makes the field experimentable and testable.
//
// Only keys the plugin's schema actually declares are injected. A plugin that
// defines `doctor` without declaring `required_tools` is not asking for the
// injection, and adding a field its type checker never saw would be a hole in
// §5.7's promise that `config` matches the schema.
PluginConfig for_plugin(const kpl::Plugin&                       plugin,
                        const std::string&                       plugin_name,
                        const Merged&                            merged,
                        const std::vector<std::string>&          set_values,
                        const std::map<std::string, kpl::Value>& injected = {});

// Write one dotted key into a TOML file, creating it (and any intermediate
// tables) if needed — `kap config set`. The value is stored as a string unless
// it parses as an integer or a boolean, matching what a user typing
// `kap config set detect.max_walk_up 3` obviously means.
//
// Comments and layout in an existing file are NOT preserved; see the note on
// toml::write. Throws diag::Error if the file exists and does not parse,
// because rewriting a file we did not understand would destroy it.
void set_key(const std::filesystem::path& file,
             const std::string&           dotted_key,
             const std::string&           value);

} // namespace config
} // namespace kap
