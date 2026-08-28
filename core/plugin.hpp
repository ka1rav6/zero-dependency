#pragma once

// core/plugin.hpp
//
// Finding plugins on disk and running their fixture tests — the parts of
// design doc §6 that Milestone 3 needs, and no more. The full plugin manager
// (install from a registry or a git URL, lockfile, enable/disable, pinning)
// arrives in Milestone 7 and will build on `discover()` rather than replace it.
//
// A plugin is a directory containing `plugin.kpl` (§5.2). Today only the
// bundled first-party plugins under `<root>/plugins/` are searched; §6.5's
// three-level override precedence (project-local > user-installed > bundled)
// is a Milestone-7 concern.

#include <filesystem>
#include <string>
#include <vector>

namespace kap
{
namespace plugin
{

// One plugin found on disk.
struct Located
{
    std::string           name;      // the directory name, e.g. "cmake-cpp"
    std::filesystem::path directory; // <root>/plugins/cmake-cpp
    std::filesystem::path manifest;  // <root>/plugins/cmake-cpp/plugin.kpl
};

// Every plugin directory under `<root>/plugins`, sorted by name so output is
// deterministic. Directories without a `plugin.kpl` are skipped rather than
// reported: a stray directory is not a broken plugin.
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
std::vector<CaseResult> run_tests(const Located& plugin);

} // namespace plugin
} // namespace kap
