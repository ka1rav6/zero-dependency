#pragma once

// core/plugin.hpp
//
// Finding plugins on disk and running their fixture tests (design doc §6.5 and
// §5.2). The full plugin manager — install from a registry or a git URL, the
// lockfile, enable/disable, pinning — lives in core/registry.hpp and builds on
// `discover()` rather than replacing it.
//
// A plugin is a directory containing `plugin.kpl` (§5.2). Which directories
// are searched, and in what order, is §6.5's override precedence; see
// `DiscoveryOptions` below for the exact list.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace kap
{
namespace plugin
{

// Where a plugin was found. The order of the enumerators *is* §6.5's override
// precedence: a project-local plugin shadows a user-installed one of the same
// name, which shadows one bundled with the binary.
enum class Source
{
    ProjectLocal, // <root>/.kap/plugins/<name>/
    SearchPath,   // an entry of $KAP_PLUGIN_PATH
    User,         // ~/.local/share/kap/plugins/<name>/
    Bundled,      // <prefix>/share/kap/plugins/<name>/
    Repository,   // <root>/kap-plugins/<name>/ or <root>/plugins/<name>/ — see
                  // DiscoveryOptions::include_repository
    Embedded,     // compiled into the binary, materialized into the cache
};

// A human-readable name for a Source, used by `kap plugin list`.
const char* source_name(Source source);

// One plugin found on disk.
struct Located
{
    std::string           name;      // the directory name, e.g. "cmake-cpp"
    std::filesystem::path directory; // .../plugins/cmake-cpp
    std::filesystem::path manifest;  // .../plugins/cmake-cpp/plugin.kpl
    Source                source = Source::Repository;

    // Set by the lockfile layer (§6.1 `kap plugin enable/disable`). Discovery
    // itself reports every plugin it finds and leaves the policy to the
    // caller, so `kap plugin list` can show a disabled plugin as disabled
    // instead of pretending it does not exist.
    bool enabled = true;

    // Copies of this same plugin in lower-precedence tiers, in the order they
    // were found. Shadowing is correct behaviour (§6.5) and usually invisible
    // on purpose — but *silently* invisible is how someone edits a plugin in a
    // checkout, sees no change because an installed copy outranks it, and
    // concludes the tool is broken. Recording the losers lets the caller say
    // so; `shadowing()` below picks out the one case worth interrupting for.
    std::vector<std::pair<Source, std::filesystem::path>> shadowed;

    // True when this plugin hides a copy in the *repository* the user is
    // standing in. That specific collision means "your edits are being
    // ignored", which is a mistake rather than a configuration, so it is the
    // only one kap volunteers without being asked.
    bool shadows_repository() const
    {
        for (const auto& [source, path] : shadowed) {
            (void) path;
            if (source == Source::Repository)
                return true;
        }
        return false;
    }
};

// Which directories `discover()` searches. Every field defaults to the
// behaviour a real `kap` invocation wants; tests turn the machine-wide tiers
// off so a plugin the developer happens to have installed cannot change a test
// result.
struct DiscoveryOptions
{
    // The project root. Supplies the ProjectLocal tier (<root>/.kap/plugins)
    // and, when `include_repository` is set, the Repository tier.
    std::filesystem::path project_root;

    // <root>/plugins — not one of §6.5's three tiers, but the directory a
    // checkout of kap itself (and any repository that develops plugins
    // in-tree) keeps its plugins in. Searched last, so an installed plugin of
    // the same name always wins. This is what makes `kap plugin doctor
    // --root .` work in a fresh clone with nothing installed.
    bool include_repository = true;

    // $KAP_PLUGIN_PATH, colon-separated. Read from the environment unless
    // `search_path` below is set, which is how tests pin it.
    bool include_search_path = true;

    // ~/.local/share/kap/plugins (§6.4).
    bool include_user = true;

    // <prefix>/share/kap/plugins, next to the binary (§6.5).
    bool include_bundled = true;

    // Plugins compiled into the binary (a -DKAP_EMBED_PLUGINS=ON build). They
    // are written into ~/.cache/kap/embedded on first use and then treated like
    // any other directory — see core/bundled.hpp for why materializing beats
    // interpreting them from memory.
    //
    // Searched *last*, below even the repository tier. An embedded plugin is a
    // snapshot taken when the binary was compiled, so it is the weakest claim
    // of all: a distributor's patched copy should win, and so should the
    // plugins in the checkout a developer is editing. Putting it above the
    // repository tier made an embedded kap ignore the very plugin.kpl its user
    // had open.
    bool include_embedded = true;

    // When non-empty, replaces $KAP_PLUGIN_PATH entirely.
    std::vector<std::filesystem::path> search_path;
};

// Every plugin visible from `options`, sorted by name.
//
// A name found in more than one tier is reported once, from the
// highest-precedence tier that has it (§6.5). Directories without a
// `plugin.kpl` are skipped rather than reported: a stray directory is not a
// broken plugin.
std::vector<Located> discover(const DiscoveryOptions& options);

// Convenience overload: discover with default options rooted at `root`.
std::vector<Located> discover(const std::filesystem::path& root);

// The outcome of one fixture test case (`kap plugin test`).
struct CaseResult
{
    std::string name; // "<fixture>.<command>"
    bool        passed = false;
    std::string detail; // empty when passed; otherwise the expected/actual diff
};

// Run every fixture test case a plugin declares.
//
// The layout (design doc §5.2, with the fixture-to-expectation binding made
// explicit — see docs/design.md §5.2):
//
//     <plugin>/tests/fixtures/<fixture>/          the project tree to evaluate against
//     <plugin>/tests/expected/<fixture>.<command>.steps.json
//
// The file name names both halves of the case, so a plugin can have many
// fixtures and many commands without any extra manifest file. The JSON is a
// §5.4 CommandSpec, optionally preceded by the inputs the case needs:
//
//     {
//       "config": { "build_dir": "out" },   // overrides on the schema defaults
//       "extra":  ["--release"],            // passthrough arguments
//       "tools":  ["ninja"],                // what project.tool() reports present
//       "env":    { "CI": "true" },         // what project.env() returns
//       "steps":  [ ... ], "concurrent": false, "report_freed_space": false
//     }
//
// `tools` and `env` are declared rather than read from the host because §5.1
// promises the same inputs always yield the same CommandSpec. A test that
// consulted the real PATH would pass or fail depending on whose laptop it ran
// on. Everything else — exists, read, glob — comes from the fixture directory
// itself, which is what a fixture is for.
//
// A plugin that parses but declares no cases returns an empty vector; the
// caller decides whether that is worth reporting.
//
// `cache_directory` is the KPL AST cache (design doc §5.14); pass an empty
// path to bypass it. Bypassing changes nothing observable — a cache hit and a
// fresh parse produce the same AST — so tests that want to isolate the
// interpreter can pass {} and tests that want to exercise the cache can point
// it at a scratch directory.
std::vector<CaseResult> run_tests(const Located&               plugin,
                                  const std::filesystem::path& cache_directory = {});

} // namespace plugin
} // namespace kap
