#pragma once

// core/detect.hpp
//
// The detection engine (design doc §3 and Milestone 4). Given a directory and
// the plugins installed on this machine, it answers one question: *which
// plugin owns this project?*
//
// The engine has no built-in knowledge of any ecosystem — it does not know
// that Cargo.toml means Rust. Every rule it evaluates is declared by a plugin
// in its `detect` block (§3.1), which is why adding support for a new build
// system never requires rebuilding kap.
//
// ## Shape of the work
//
// A plugin's `detect` block is *compiled once* into a `RuleTable` (§5.14: "
// detection rules are extracted from the AST at load time into a C++
// DetectRuleTable so the hot path never re-parses text"). Evaluating a
// RuleTable is then a handful of stat() calls with no interpreter involved.
//
// ## Why resolution is a separate step from matching
//
// Several plugins can match one directory — a Rust workspace that also has a
// CMakeLists.txt for its C shim, a monorepo with both. §3.2 step 4 is
// deliberate about what happens then: honour an explicit pin, else the highest
// `priority`, and on a genuine tie *fail* rather than guess. Keeping
// resolution separate from rule evaluation is what makes that policy readable
// and testable on its own.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/diag.hpp"
#include "core/kpl.hpp"
#include "core/plugin.hpp"

namespace kap
{
namespace detect
{

// The highest KPL `api_version` this build understands (design doc §12 Q2).
// A plugin declaring more than this is refused where it is named explicitly
// (`kap plugin install/doctor/test`) and skipped-with-a-warning during
// detection, so one too-new plugin sitting in the plugin directory cannot
// break `kap build` for an unrelated project.
inline constexpr int kSupportedApiVersion = 1;

// One compiled detection rule (§3.2 step 2).
struct Rule
{
    enum class Kind
    {
        FileExists,    // file_exists "Cargo.toml"
        FileExistsAny, // file_exists_any ["*.xcworkspace", "*.xcodeproj"]
        DirExists,     // dir_exists ".git/modules"
        FileContains,  // file_contains { path: "package.json", pattern: "\"workspaces\"" }
    };

    Kind kind = Kind::FileExists;

    // FileExists/DirExists use exactly one pattern; FileExistsAny uses all of
    // them; FileContains uses the first as its path and `pattern` as the
    // needle. One vector rather than a variant keeps the compiled table flat.
    std::vector<std::string> patterns;
    std::string              pattern; // FileContains only: the substring to find

    // Where the rule was written, so a malformed one can be reported at its
    // own line rather than "somewhere in this plugin".
    diag::Location location;
};

// A plugin's whole detection surface, extracted from its AST once.
struct RuleTable
{
    std::string              name;
    std::string              version;
    int                      api_version = 1;
    int                      priority    = 0;
    bool                     composable  = false;
    std::vector<std::string> supersedes;
    std::vector<Rule>        rules;
};

// Compile a parsed plugin's `manifest` + `detect` blocks into a RuleTable.
// `fallback_name` is used when the manifest omits `name` (the directory name
// is a better answer than an empty string). Throws diag::Error on a detect
// directive the engine does not recognise or cannot make sense of — a silently
// ignored rule would mean a plugin that never matches for no visible reason.
RuleTable compile(const kpl::Plugin& plugin, const std::string& fallback_name = {});

// Evaluate one rule against `root`. Any marker path that fired is appended to
// `matched_files` (§3.4 exposes those to the plugin as `project.matched_files`).
bool evaluate(const Rule&                  rule,
              const std::filesystem::path& root,
              std::vector<std::string>&    matched_files);

// One plugin that matched.
struct Match
{
    plugin::Located located;
    std::string     name;
    int             priority   = 0;
    bool            composable = false;

    // How many of the plugin's rules fired (§3.2 step 2: "each satisfied rule
    // contributes a match score"). Reported by `kap detect`; ranking is by
    // `priority`, so score is diagnostic rather than decisive.
    int                      score = 0;
    std::vector<std::string> matched_files;
};

// Inputs to resolution that come from configuration rather than from disk.
struct Options
{
    // detect.max_walk_up (§3.2 step 1). 0 means "this directory only"; a
    // positive value permits walking toward the filesystem root, stopping at
    // the first directory where anything matches.
    int max_walk_up = 0;

    // detect.ecosystem from kap.toml (§3.2 step 4): when set, this plugin wins
    // outright if it matched, and the tie path is never reached.
    std::string pinned;

    // .kap/cache.json (§3.2 step 5). Reading and writing are separate switches
    // because `kap detect --refresh` wants to ignore an existing entry while
    // still leaving a fresh one behind.
    bool read_cache  = true;
    bool write_cache = true;

    // The KPL AST cache directory (§5.14). Empty disables it.
    std::filesystem::path ast_cache;
};

// The outcome of a resolution.
struct Resolution
{
    // The directory detection settled on. Equal to the starting directory
    // unless `max_walk_up` let the engine walk upward (§3.2 step 1).
    std::filesystem::path root;

    // The winning plugin first, then every `composable: true` plugin that also
    // matched (§3.3), in descending priority. Empty when nothing matched.
    std::vector<Match> matches;

    // True when the whole answer came from .kap/cache.json without a scan.
    bool from_cache = false;

    // Non-fatal problems: a plugin that failed to parse, or one whose
    // api_version is newer than this kap supports. Detection continues without
    // it, and the caller decides whether to print these (they go to stderr
    // under --verbose, and on a failed detection where they may be the cause).
    std::vector<std::string> warnings;

    bool matched() const
    {
        return !matches.empty();
    }

    // The plugin that owns the ordinary commands (build, test, ...), or
    // nullptr when only composable sidecars matched.
    const Match* primary() const
    {
        for (const Match& match : matches)
            if (!match.composable)
                return &match;
        return nullptr;
    }
};

// Resolve `start` against `plugins` (§3.2 in full).
//
// Throws diag::Error only for the one case §3.2 step 4 says must never be
// guessed at: two or more non-composable plugins matching at the same
// priority with no pin to break the tie. Everything else — no match, an
// unparsable plugin, an unreadable cache — is reported through the returned
// Resolution.
Resolution resolve(const std::filesystem::path&        start,
                   const std::vector<plugin::Located>& plugins,
                   const Options&                      options = {});

// The cache file for a project root: <root>/.kap/cache.json (§3.2 step 5).
std::filesystem::path cache_file(const std::filesystem::path& root);

// Delete a project's detection cache. Called after a plugin is installed or
// removed (§6.3 step 7), where the set of candidates has changed underneath
// every cached answer on the machine. Missing file is success, not an error.
void invalidate_cache(const std::filesystem::path& root);

} // namespace detect
} // namespace kap
