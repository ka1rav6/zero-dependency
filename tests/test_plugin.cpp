// tests/test_plugin.cpp
//
// Unit tests for plugin discovery and the fixture-test runner (core/plugin.hpp,
// design doc §6.1 and Milestone 3's exit criterion).
//
// Each test builds a throwaway plugin tree under the system temp directory, so
// nothing here depends on the repository's own plugins/ — those are covered
// end to end by tests/e2e.sh instead.

#include "core/diag.hpp"
#include "core/plugin.hpp"
#include "harness.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{

std::filesystem::path scratch_root(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_plugin_" + name + "_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// Discover only what this test wrote.
//
// The bare `discover(root)` overload searches every tier §6.5 defines —
// including the user's installed plugins and, in a -DKAP_EMBED_PLUGINS=ON
// build, the plugins compiled into the binary. A unit test that used it would
// pass or fail depending on what the developer happens to have installed and
// how kap was configured, which is not a property a test should have.
//
// Switching the machine-wide tiers off leaves exactly the scratch tree below.
std::vector<kap::plugin::Located> discover_only(const std::filesystem::path& root)
{
    kap::plugin::DiscoveryOptions options;
    options.project_root        = root;
    options.include_search_path = false;
    options.include_user        = false;
    options.include_bundled     = false;
    options.include_embedded    = false;
    return kap::plugin::discover(options);
}

// A minimal but complete plugin: manifest, detect rule, schema, one command.
const char* kEchoPlugin = R"(
manifest {
  name        = "echo"
  version     = "1.0.0"
  api_version = 1
}

detect {
  file_exists "marker.txt"
}

schema {
  greeting: str = "hello"
}

command build(project, config, extra) {
  step echo config.greeting
  if project.exists("marker.txt") { step echo "found-marker" }
  step ["echo"] + extra
}
)";

} // namespace

KAP_TEST("discover finds plugin directories and skips non-plugins")
{
    const std::filesystem::path root = scratch_root("discover");
    write_file(root / "plugins" / "beta" / "plugin.kpl", kEchoPlugin);
    write_file(root / "plugins" / "alpha" / "plugin.kpl", kEchoPlugin);
    // A directory with no plugin.kpl is not a broken plugin, it is not a
    // plugin at all — an editor scratch directory must not be reported.
    std::filesystem::create_directories(root / "plugins" / "notes");
    write_file(root / "plugins" / "loose.txt", "ignored");

    const auto found = discover_only(root);
    KAP_ASSERT_EQ(found.size(), static_cast<std::size_t>(2));
    // Sorted, so output and test expectations are stable.
    KAP_ASSERT_EQ(found[0].name, std::string("alpha"));
    KAP_ASSERT_EQ(found[1].name, std::string("beta"));
    KAP_ASSERT_EQ(found[0].manifest.filename().string(), std::string("plugin.kpl"));

    std::filesystem::remove_all(root);
});

KAP_TEST("discover returns nothing for a root with no plugins directory")
{
    const std::filesystem::path root = scratch_root("empty");
    KAP_ASSERT(discover_only(root).empty());
    std::filesystem::remove_all(root);
});

KAP_TEST("plugin tests pass when the spec matches the golden file")
{
    const std::filesystem::path root = scratch_root("pass");
    const std::filesystem::path dir  = root / "plugins" / "echo";
    write_file(dir / "plugin.kpl", kEchoPlugin);
    write_file(dir / "tests" / "fixtures" / "demo" / "marker.txt", "x");
    write_file(dir / "tests" / "expected" / "demo.build.steps.json", R"({
      "config": { "greeting": "hi" },
      "extra": ["--flag"],
      "steps": [
        { "cmd": ["echo", "hi"] },
        { "cmd": ["echo", "found-marker"] },
        { "cmd": ["echo", "--flag"] }
      ]
    })");

    const auto results = kap::plugin::run_tests(discover_only(root).front());
    KAP_ASSERT_EQ(results.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(results[0].passed);
    KAP_ASSERT_EQ(results[0].name, std::string("demo.build"));

    std::filesystem::remove_all(root);
});

KAP_TEST("plugin tests read the real fixture tree through the sandbox")
{
    // The same plugin against a fixture *without* the marker file takes the
    // other branch, which proves project.exists really consults the fixture
    // rather than a mock.
    const std::filesystem::path root = scratch_root("fixture");
    const std::filesystem::path dir  = root / "plugins" / "echo";
    write_file(dir / "plugin.kpl", kEchoPlugin);
    write_file(dir / "tests" / "fixtures" / "bare" / "other.txt", "x");
    write_file(dir / "tests" / "expected" / "bare.build.steps.json", R"({
      "steps": [ { "cmd": ["echo", "hello"] }, { "cmd": ["echo"] } ]
    })");

    const auto results = kap::plugin::run_tests(discover_only(root).front());
    KAP_ASSERT_EQ(results.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(results[0].passed);

    std::filesystem::remove_all(root);
});

KAP_TEST("plugin tests fail with both renderings when the spec differs")
{
    const std::filesystem::path root = scratch_root("diff");
    const std::filesystem::path dir  = root / "plugins" / "echo";
    write_file(dir / "plugin.kpl", kEchoPlugin);
    write_file(dir / "tests" / "fixtures" / "demo" / "marker.txt", "x");
    write_file(dir / "tests" / "expected" / "demo.build.steps.json", R"({
      "steps": [ { "cmd": ["echo", "WRONG"] } ]
    })");

    const auto results = kap::plugin::run_tests(discover_only(root).front());
    KAP_ASSERT_EQ(results.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(!results[0].passed);
    // Both sides are shown, because "it differs" is not actionable on its own.
    KAP_ASSERT(results[0].detail.find("WRONG") != std::string::npos);
    KAP_ASSERT(results[0].detail.find("found-marker") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("plugin tests declare tools and env rather than sampling the host")
{
    // Design doc §5.1 promises the same inputs always produce the same
    // CommandSpec. A case that consulted the real PATH would pass or fail
    // depending on whose machine ran it, so both are declared in the case file.
    const std::filesystem::path root = scratch_root("declared");
    const std::filesystem::path dir  = root / "plugins" / "probe";
    write_file(dir / "plugin.kpl", R"(
manifest { name = "probe" version = "1.0.0" api_version = 1 }
command build(project, config, extra) {
  if project.tool("ninja") { step echo "ninja" } else { step echo "make" }
  if project.env("CI") != none { step echo "ci" }
}
)");
    write_file(dir / "tests" / "fixtures" / "demo" / ".keep", "");
    write_file(dir / "tests" / "expected" / "demo.build.steps.json", R"({
      "tools": ["ninja"],
      "env": { "CI": "true" },
      "steps": [ { "cmd": ["echo", "ninja"] }, { "cmd": ["echo", "ci"] } ]
    })");
    write_file(dir / "tests" / "expected" / "no-tools.steps.json", R"({
      "fixture": "demo",
      "command": "build",
      "steps": [ { "cmd": ["echo", "make"] } ]
    })");

    const auto results = kap::plugin::run_tests(discover_only(root).front());
    KAP_ASSERT_EQ(results.size(), static_cast<std::size_t>(2));
    for (const auto& result : results)
        KAP_ASSERT(result.passed);

    std::filesystem::remove_all(root);
});

KAP_TEST("plugin tests report a plugin that does not parse or type-check")
{
    const std::filesystem::path root = scratch_root("broken");
    write_file(root / "plugins" / "bad-syntax" / "plugin.kpl", "manifest { name = \n");
    write_file(root / "plugins" / "bad-types" / "plugin.kpl",
               "manifest { name = \"t\" version = \"1\" api_version = 1 }\n"
               "command build(project) { if \"yes\" { step \"x\" } }\n");

    const auto found = discover_only(root);
    KAP_ASSERT_EQ(found.size(), static_cast<std::size_t>(2));

    const auto syntax = kap::plugin::run_tests(found[0]);
    KAP_ASSERT_EQ(syntax.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(!syntax[0].passed);
    KAP_ASSERT_EQ(syntax[0].name, std::string("<parse>"));

    // A plugin that cannot type-check is reported once, not once per case:
    // every case would fail for the same reason.
    const auto types = kap::plugin::run_tests(found[1]);
    KAP_ASSERT_EQ(types.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(!types[0].passed);
    KAP_ASSERT_EQ(types[0].name, std::string("<typecheck>"));
    KAP_ASSERT(types[0].detail.find("boolean") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("plugin tests report malformed cases instead of skipping them")
{
    const std::filesystem::path root = scratch_root("malformed");
    const std::filesystem::path dir  = root / "plugins" / "echo";
    write_file(dir / "plugin.kpl", kEchoPlugin);
    write_file(dir / "tests" / "fixtures" / "demo" / "marker.txt", "x");

    // A case naming a fixture that does not exist.
    write_file(dir / "tests" / "expected" / "missing.build.steps.json",
               R"({ "steps": [ { "cmd": ["echo"] } ] })");
    // A case naming a command the plugin does not define.
    write_file(dir / "tests" / "expected" / "demo.nosuch.steps.json",
               R"({ "steps": [ { "cmd": ["echo"] } ] })");
    // A case whose config key is not in the schema.
    write_file(dir / "tests" / "expected" / "demo.build.steps.json",
               R"({ "config": { "nope": 1 }, "steps": [ { "cmd": ["echo"] } ] })");
    // A case that is not valid JSON at all.
    write_file(dir / "tests" / "expected" / "demo.broken.steps.json", "{ not json");

    const auto results = kap::plugin::run_tests(discover_only(root).front());
    KAP_ASSERT_EQ(results.size(), static_cast<std::size_t>(4));
    for (const auto& result : results)
        KAP_ASSERT(!result.passed);

    std::filesystem::remove_all(root);
});

KAP_TEST("a plugin with no cases yields no results")
{
    const std::filesystem::path root = scratch_root("nocases");
    write_file(root / "plugins" / "echo" / "plugin.kpl", kEchoPlugin);
    KAP_ASSERT(kap::plugin::run_tests(discover_only(root).front()).empty());
    std::filesystem::remove_all(root);
});

KAP_TEST("a case with no fixture name uses the only fixture, or reports ambiguity")
{
    const std::filesystem::path root = scratch_root("implicit");
    const std::filesystem::path dir  = root / "plugins" / "echo";
    write_file(dir / "plugin.kpl", kEchoPlugin);
    write_file(dir / "tests" / "fixtures" / "only" / "marker.txt", "x");
    // Design doc §5.2 writes exactly this file name, with no fixture in it.
    write_file(dir / "tests" / "expected" / "build.steps.json", R"({
      "steps": [
        { "cmd": ["echo", "hello"] },
        { "cmd": ["echo", "found-marker"] },
        { "cmd": ["echo"] }
      ]
    })");

    const auto single = kap::plugin::run_tests(discover_only(root).front());
    KAP_ASSERT_EQ(single.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(single[0].passed);

    // Add a second fixture and the same file becomes ambiguous, which is
    // reported rather than guessed at.
    write_file(dir / "tests" / "fixtures" / "second" / "marker.txt", "x");
    const auto ambiguous = kap::plugin::run_tests(discover_only(root).front());
    KAP_ASSERT_EQ(ambiguous.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(!ambiguous[0].passed);
    KAP_ASSERT(ambiguous[0].detail.find("no fixture named") != std::string::npos);

    std::filesystem::remove_all(root);
});
