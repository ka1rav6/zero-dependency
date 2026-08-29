// tests/test_detect.cpp
//
// Unit tests for the detection engine (core/detect.hpp, design doc §3 and
// Milestone 4).
//
// Every test builds a throwaway project tree plus its own plugin directory
// under the system temp directory, and hands the plugin list to
// detect::resolve() explicitly. Nothing here reads the developer's installed
// plugins, so a plugin someone happens to have on their machine can never
// change a result.
//
// Each rule kind, each resolution rule, and each cache-invalidation trigger is
// a separate test (AGENTS.md §6: "test every feature separately so failures
// are isolated and diagnosable").

#include "core/detect.hpp"
#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/kpl.hpp"
#include "core/plugin.hpp"
#include "harness.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace
{

// A fresh, empty directory named after the test. Deleted first so a crashed
// previous run cannot leak state into this one.
std::filesystem::path scratch_root(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_detect_" + name + "_" + std::to_string(getpid()));
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

// Write a plugin into `<dir>/plugins/<name>/plugin.kpl` and return its Located.
// Tests build the plugin list by hand rather than calling discover(), so the
// engine is exercised in isolation from the search-path rules.
kap::plugin::Located
make_plugin(const std::filesystem::path& dir, const std::string& name, const std::string& source)
{
    kap::plugin::Located located;
    located.name      = name;
    located.directory = dir / "plugins" / name;
    located.manifest  = located.directory / "plugin.kpl";
    located.source    = kap::plugin::Source::Repository;
    write_file(located.manifest, source);
    return located;
}

// A plugin whose whole content is a manifest and one detect block.
std::string simple_plugin(const std::string& name,
                          int                priority,
                          const std::string& detect_body,
                          const std::string& extra_manifest = {})
{
    return "manifest {\n  name = \"" + name +
           "\"\n  version = \"1.0.0\"\n  api_version = 1\n  priority = " +
           std::to_string(priority) + "\n" + extra_manifest + "}\ndetect {\n" + detect_body +
           "\n}\n";
}

// Options with both halves of the cache off. Most tests are about the rules,
// not the cache, and a cache left on would make them order-dependent.
kap::detect::Options no_cache()
{
    kap::detect::Options options;
    options.read_cache  = false;
    options.write_cache = false;
    return options;
}

} // namespace

// --- rule compilation ------------------------------------------------------------

KAP_TEST("compile reads the manifest fields the engine ranks on")
{
    const kap::kpl::Plugin plugin = kap::kpl::parse(
        simple_plugin("demo",
                      42,
                      R"(  file_exists "Cargo.toml")",
                      "  composable = true\n  supersedes = [\"other\", \"another\"]\n"));
    const kap::detect::RuleTable table = kap::detect::compile(plugin);

    KAP_ASSERT_EQ(table.name, std::string("demo"));
    KAP_ASSERT_EQ(table.priority, 42);
    KAP_ASSERT_EQ(table.api_version, 1);
    KAP_ASSERT(table.composable);
    KAP_ASSERT_EQ(table.supersedes.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(table.supersedes[0], std::string("other"));
    KAP_ASSERT_EQ(table.rules.size(), static_cast<std::size_t>(1));
});

KAP_TEST("compile falls back to the directory name when the manifest omits one")
{
    const kap::kpl::Plugin plugin =
        kap::kpl::parse("manifest {\n  version = \"1.0.0\"\n}\ndetect {\n  file_exists \"x\"\n}\n");
    KAP_ASSERT_EQ(kap::detect::compile(plugin, "from-directory").name,
                  std::string("from-directory"));
});

KAP_TEST("compile rejects an unknown detect rule instead of ignoring it")
{
    const kap::kpl::Plugin plugin =
        kap::kpl::parse(simple_plugin("demo", 1, R"(  file_smells "Cargo.toml")"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::detect::compile(plugin));
});

KAP_TEST("compile rejects a detect rule given the wrong argument shape")
{
    // file_exists wants one string; a list is a different rule's argument.
    const kap::kpl::Plugin list_arg =
        kap::kpl::parse(simple_plugin("demo", 1, R"(  file_exists ["a", "b"])"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::detect::compile(list_arg));

    // file_exists_any wants a list; a bare string is not one.
    const kap::kpl::Plugin string_arg =
        kap::kpl::parse(simple_plugin("demo", 1, R"(  file_exists_any "a")"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::detect::compile(string_arg));

    // An empty list can never match, so it is a mistake worth reporting.
    const kap::kpl::Plugin empty_list =
        kap::kpl::parse(simple_plugin("demo", 1, R"(  file_exists_any [])"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::detect::compile(empty_list));

    // file_contains needs both fields.
    const kap::kpl::Plugin half_record =
        kap::kpl::parse(simple_plugin("demo", 1, R"(  file_contains { path: "a" })"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::detect::compile(half_record));
});

KAP_TEST("a compile error names the line the rule is written on")
{
    const kap::kpl::Plugin plugin = kap::kpl::parse(
        simple_plugin("demo", 1, "  file_exists \"ok\"\n  nonsense \"bad\""), "demo/plugin.kpl");
    try {
        (void) kap::detect::compile(plugin);
        KAP_ASSERT(false); // should have thrown
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT_EQ(error.diagnostic().location.file, std::string("demo/plugin.kpl"));
        KAP_ASSERT(error.diagnostic().location.has_position());
    }
});

// --- rule evaluation, one test per rule kind ---------------------------------------

KAP_TEST("file_exists matches a regular file under the root")
{
    const std::filesystem::path root = scratch_root("file-exists");
    write_file(root / "Cargo.toml", "[package]\n");

    const kap::detect::RuleTable table = kap::detect::compile(
        kap::kpl::parse(simple_plugin("r", 1, R"(  file_exists "Cargo.toml")")));

    std::vector<std::string> markers;
    KAP_ASSERT(kap::detect::evaluate(table.rules.front(), root, markers));
    KAP_ASSERT_EQ(markers.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(markers.front(), std::string("Cargo.toml"));

    std::vector<std::string>     absent;
    const kap::detect::RuleTable missing = kap::detect::compile(
        kap::kpl::parse(simple_plugin("r", 1, R"(  file_exists "not-here.toml")")));
    KAP_ASSERT(!kap::detect::evaluate(missing.rules.front(), root, absent));
    KAP_ASSERT(absent.empty());

    std::filesystem::remove_all(root);
});

KAP_TEST("dir_exists matches a directory but not a file of the same name")
{
    const std::filesystem::path root = scratch_root("dir-exists");
    std::filesystem::create_directories(root / ".git" / "modules");
    write_file(root / "plain", "x");

    const kap::detect::RuleTable dir = kap::detect::compile(
        kap::kpl::parse(simple_plugin("r", 1, R"(  dir_exists ".git/modules")")));
    std::vector<std::string> markers;
    KAP_ASSERT(kap::detect::evaluate(dir.rules.front(), root, markers));

    const kap::detect::RuleTable file =
        kap::detect::compile(kap::kpl::parse(simple_plugin("r", 1, R"(  dir_exists "plain")")));
    std::vector<std::string> none;
    KAP_ASSERT(!kap::detect::evaluate(file.rules.front(), root, none));

    std::filesystem::remove_all(root);
});

KAP_TEST("file_exists_any matches the first alternative present")
{
    const std::filesystem::path root = scratch_root("exists-any");
    write_file(root / "b.txt", "x");

    const kap::detect::RuleTable table = kap::detect::compile(
        kap::kpl::parse(simple_plugin("r", 1, R"(  file_exists_any ["a.txt", "b.txt"])")));
    std::vector<std::string> markers;
    KAP_ASSERT(kap::detect::evaluate(table.rules.front(), root, markers));
    KAP_ASSERT_EQ(markers.front(), std::string("b.txt"));

    std::filesystem::remove_all(root);
});

KAP_TEST("file_exists_any expands a wildcard in the final path component")
{
    // §3.2's own example is ["*.xcworkspace", "*.xcodeproj"], so the wildcard
    // form is part of the contract, not a convenience.
    const std::filesystem::path root = scratch_root("exists-any-glob");
    write_file(root / "Demo.xcodeproj", "x");

    const kap::detect::RuleTable table = kap::detect::compile(kap::kpl::parse(
        simple_plugin("r", 1, R"(  file_exists_any ["*.xcworkspace", "*.xcodeproj"])")));
    std::vector<std::string>     markers;
    KAP_ASSERT(kap::detect::evaluate(table.rules.front(), root, markers));
    KAP_ASSERT_EQ(markers.front(), std::string("Demo.xcodeproj"));

    std::filesystem::remove_all(root);
});

KAP_TEST("file_contains matches only when the needle is in the file")
{
    const std::filesystem::path root = scratch_root("file-contains");
    write_file(root / "package.json", R"({ "workspaces": ["packages/*"] })");
    write_file(root / "other.json", R"({ "name": "x" })");

    const kap::detect::RuleTable hit = kap::detect::compile(kap::kpl::parse(simple_plugin(
        "r", 1, R"(  file_contains { path: "package.json", pattern: "workspaces" })")));
    std::vector<std::string>     markers;
    KAP_ASSERT(kap::detect::evaluate(hit.rules.front(), root, markers));
    KAP_ASSERT_EQ(markers.front(), std::string("package.json"));

    const kap::detect::RuleTable miss = kap::detect::compile(kap::kpl::parse(
        simple_plugin("r", 1, R"(  file_contains { path: "other.json", pattern: "workspaces" })")));
    std::vector<std::string>     none;
    KAP_ASSERT(!kap::detect::evaluate(miss.rules.front(), root, none));

    // A missing file is a non-match, not an error.
    const kap::detect::RuleTable absent = kap::detect::compile(kap::kpl::parse(
        simple_plugin("r", 1, R"(  file_contains { path: "gone.json", pattern: "x" })")));
    std::vector<std::string>     still_none;
    KAP_ASSERT(!kap::detect::evaluate(absent.rules.front(), root, still_none));

    std::filesystem::remove_all(root);
});

KAP_TEST("a detect rule cannot probe outside the project root")
{
    // Design doc §7: a plugin is untrusted. A detect rule runs before any
    // interpreter sandbox exists, so the escape check has to live in the
    // engine — otherwise `file_exists "../secret"` would let a plugin map the
    // filesystem above a project just by watching which plugin wins.
    const std::filesystem::path root = scratch_root("escape");
    write_file(root.parent_path() / "kap_detect_escape_sibling.txt", "x");
    std::filesystem::create_directories(root / "inside");

    for (const char* pattern : {R"(  file_exists "../kap_detect_escape_sibling.txt")",
                                R"(  file_exists "/etc/passwd")",
                                R"(  dir_exists "..")"}) {
        const kap::detect::RuleTable table =
            kap::detect::compile(kap::kpl::parse(simple_plugin("r", 1, pattern)));
        std::vector<std::string> markers;
        KAP_ASSERT(!kap::detect::evaluate(table.rules.front(), root, markers));
    }

    std::filesystem::remove_all(root);
    std::filesystem::remove(root.parent_path() / "kap_detect_escape_sibling.txt");
});

// --- resolution ---------------------------------------------------------------------

KAP_TEST("resolution picks the highest-priority matching plugin")
{
    const std::filesystem::path root = scratch_root("priority");
    write_file(root / "Cargo.toml", "[package]\n");
    write_file(root / "CMakeLists.txt", "project(x)\n");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cmake-cpp", simple_plugin("cmake-cpp", 30, R"(  file_exists "CMakeLists.txt")")),
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT(resolution.matched());
    KAP_ASSERT_EQ(resolution.matches.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(resolution.matches.front().name, std::string("cargo-rust"));
    KAP_ASSERT_EQ(resolution.matches.front().priority, 40);

    std::filesystem::remove_all(root);
});

KAP_TEST("score counts satisfied rules and is independent of priority")
{
    const std::filesystem::path root = scratch_root("score");
    write_file(root / "a", "x");
    write_file(root / "b", "x");

    const std::vector<kap::plugin::Located> plugins = {make_plugin(
        root, "two", simple_plugin("two", 5, "  file_exists \"a\"\n  file_exists \"b\""))};

    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT_EQ(resolution.matches.front().score, 2);
    KAP_ASSERT_EQ(resolution.matches.front().priority, 5);
    KAP_ASSERT_EQ(resolution.matches.front().matched_files.size(), static_cast<std::size_t>(2));

    // Removing one marker leaves the plugin matching, with a lower score.
    std::filesystem::remove(root / "b");
    KAP_ASSERT_EQ(kap::detect::resolve(root, plugins, no_cache()).matches.front().score, 1);

    std::filesystem::remove_all(root);
});

KAP_TEST("resolution removes a plugin another matched plugin supersedes")
{
    const std::filesystem::path root = scratch_root("supersede");
    write_file(root / "marker.txt", "x");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root,
            "generic",
            simple_plugin(
                "generic", 10, R"(  file_exists "marker.txt")", "  supersedes = [\"special\"]\n")),
        make_plugin(root, "special", simple_plugin("special", 20, R"(  file_exists "marker.txt")")),
    };

    // `special` has the higher priority but `generic` supersedes it, and
    // §3.2 step 3 removes superseded plugins *before* ranking.
    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT_EQ(resolution.matches.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(resolution.matches.front().name, std::string("generic"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a genuine priority tie is an error, never a guess")
{
    const std::filesystem::path root = scratch_root("tie");
    write_file(root / "marker.txt", "x");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(root, "alpha", simple_plugin("alpha", 25, R"(  file_exists "marker.txt")")),
        make_plugin(root, "beta", simple_plugin("beta", 25, R"(  file_exists "marker.txt")")),
    };

    KAP_ASSERT_THROWS(kap::diag::Error, kap::detect::resolve(root, plugins, no_cache()));

    // The message has to be actionable: it names both candidates and shows the
    // kap.toml stanza that resolves it.
    try {
        (void) kap::detect::resolve(root, plugins, no_cache());
    }
    catch (const kap::diag::Error& error) {
        const std::string report = error.report();
        KAP_ASSERT(report.find("alpha") != std::string::npos);
        KAP_ASSERT(report.find("beta") != std::string::npos);
        KAP_ASSERT(report.find("ecosystem") != std::string::npos);
    }

    std::filesystem::remove_all(root);
});

KAP_TEST("a pin settles a tie without an error")
{
    const std::filesystem::path root = scratch_root("pin");
    write_file(root / "marker.txt", "x");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(root, "alpha", simple_plugin("alpha", 25, R"(  file_exists "marker.txt")")),
        make_plugin(root, "beta", simple_plugin("beta", 25, R"(  file_exists "marker.txt")")),
    };

    kap::detect::Options options             = no_cache();
    options.pinned                           = "beta";
    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, options);
    KAP_ASSERT_EQ(resolution.matches.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(resolution.matches.front().name, std::string("beta"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a pin naming a plugin that did not match warns rather than silently winning")
{
    const std::filesystem::path root = scratch_root("pin-miss");
    write_file(root / "marker.txt", "x");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(root, "alpha", simple_plugin("alpha", 25, R"(  file_exists "marker.txt")")),
    };

    kap::detect::Options options             = no_cache();
    options.pinned                           = "not-installed";
    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, options);

    KAP_ASSERT_EQ(resolution.matches.front().name, std::string("alpha"));
    KAP_ASSERT_EQ(resolution.warnings.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(resolution.warnings.front().find("not-installed") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("a composable plugin runs alongside the winner instead of competing")
{
    const std::filesystem::path root = scratch_root("composable");
    write_file(root / "Cargo.toml", "[package]\n");
    write_file(root / "docker-compose.yml", "services: {}\n");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
        make_plugin(
            root,
            "compose",
            simple_plugin(
                "compose", 90, R"(  file_exists "docker-compose.yml")", "  composable = true\n")),
    };

    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT_EQ(resolution.matches.size(), static_cast<std::size_t>(2));

    // Despite its far higher priority, the sidecar does not become primary.
    KAP_ASSERT(resolution.primary() != nullptr);
    KAP_ASSERT_EQ(resolution.primary()->name, std::string("cargo-rust"));
    KAP_ASSERT_EQ(resolution.matches.back().name, std::string("compose"));
    KAP_ASSERT(resolution.matches.back().composable);

    std::filesystem::remove_all(root);
});

KAP_TEST("two composable plugins never tie because neither is primary")
{
    const std::filesystem::path root = scratch_root("composable-tie");
    write_file(root / "marker.txt", "x");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root,
            "one",
            simple_plugin("one", 5, R"(  file_exists "marker.txt")", "  composable = true\n")),
        make_plugin(
            root,
            "two",
            simple_plugin("two", 5, R"(  file_exists "marker.txt")", "  composable = true\n")),
    };

    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT_EQ(resolution.matches.size(), static_cast<std::size_t>(2));
    KAP_ASSERT(resolution.primary() == nullptr);

    std::filesystem::remove_all(root);
});

KAP_TEST("nothing matching is reported, not invented")
{
    const std::filesystem::path             root    = scratch_root("no-match");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT(!resolution.matched());
    KAP_ASSERT(resolution.primary() == nullptr);

    std::filesystem::remove_all(root);
});

KAP_TEST("a plugin that fails to parse is skipped with a warning, not fatal")
{
    const std::filesystem::path root = scratch_root("broken-plugin");
    write_file(root / "Cargo.toml", "[package]\n");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(root, "broken", "manifest { this is not KPL at all ("),
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT_EQ(resolution.matches.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(resolution.matches.front().name, std::string("cargo-rust"));
    KAP_ASSERT_EQ(resolution.warnings.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(resolution.warnings.front().find("broken") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("a plugin declaring a newer api_version is skipped with a warning")
{
    // Design doc §12 Q2, resolved as: hard error where the plugin is named
    // explicitly, warn-and-skip during detection so one too-new plugin cannot
    // break `kap build` for an unrelated project.
    const std::filesystem::path root = scratch_root("api-version");
    write_file(root / "marker.txt", "x");

    const std::string too_new =
        "manifest {\n  name = \"future\"\n  version = \"9.0.0\"\n  api_version = 99\n"
        "  priority = 10\n}\ndetect {\n  file_exists \"marker.txt\"\n}\n";
    const std::vector<kap::plugin::Located> plugins = {make_plugin(root, "future", too_new)};

    const kap::detect::Resolution resolution = kap::detect::resolve(root, plugins, no_cache());
    KAP_ASSERT(!resolution.matched());
    KAP_ASSERT_EQ(resolution.warnings.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(resolution.warnings.front().find("api_version 99") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("a disabled plugin is not considered")
{
    const std::filesystem::path root = scratch_root("disabled");
    write_file(root / "Cargo.toml", "[package]\n");

    std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };
    plugins.front().enabled = false;

    KAP_ASSERT(!kap::detect::resolve(root, plugins, no_cache()).matched());

    std::filesystem::remove_all(root);
});

// --- upward walk (§3.2 step 1) --------------------------------------------------------

KAP_TEST("max_walk_up 0 means this directory and nowhere else")
{
    const std::filesystem::path root = scratch_root("walk-zero");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::filesystem::path nested = root / "src" / "deep";
    std::filesystem::create_directories(nested);

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    KAP_ASSERT(!kap::detect::resolve(nested, plugins, no_cache()).matched());

    std::filesystem::remove_all(root);
});

KAP_TEST("max_walk_up finds the project from a subdirectory and reports the real root")
{
    const std::filesystem::path root = scratch_root("walk-up");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::filesystem::path nested = root / "src" / "deep";
    std::filesystem::create_directories(nested);

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    kap::detect::Options options             = no_cache();
    options.max_walk_up                      = 2;
    const kap::detect::Resolution resolution = kap::detect::resolve(nested, plugins, options);

    KAP_ASSERT(resolution.matched());
    // The reported root is where the markers actually are — everything
    // downstream (cwd for steps, project.read) depends on this being right.
    KAP_ASSERT_EQ(resolution.root, std::filesystem::weakly_canonical(root));

    std::filesystem::remove_all(root);
});

KAP_TEST("max_walk_up stops after the configured number of levels")
{
    const std::filesystem::path root = scratch_root("walk-limit");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::filesystem::path nested = root / "a" / "b" / "c";
    std::filesystem::create_directories(nested);

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    kap::detect::Options options = no_cache();
    options.max_walk_up          = 2; // reaches root/a, one level short
    KAP_ASSERT(!kap::detect::resolve(nested, plugins, options).matched());

    options.max_walk_up = 3;
    KAP_ASSERT(kap::detect::resolve(nested, plugins, options).matched());

    std::filesystem::remove_all(root);
});

// --- the cache (§3.2 step 5) -------------------------------------------------------

namespace
{

// Give the filesystem a moment so a rewritten file gets a different mtime.
// Filesystem timestamp granularity varies (ext4 is nanoseconds, some are one
// second), and a cache test that races the clock is a flaky test.
void bump_mtime(const std::filesystem::path& path)
{
    const auto      now = std::filesystem::file_time_type::clock::now();
    std::error_code ec;
    std::filesystem::last_write_time(path, now + std::chrono::seconds(2), ec);
}

} // namespace

KAP_TEST("a second resolution comes from the cache without rescanning")
{
    const std::filesystem::path root = scratch_root("cache-hit");
    write_file(root / "Cargo.toml", "[package]\n");

    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    const kap::detect::Resolution first = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(first.matched());
    KAP_ASSERT(!first.from_cache);
    KAP_ASSERT(kap::fs::is_file(kap::detect::cache_file(root)));

    const kap::detect::Resolution second = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(second.from_cache);
    KAP_ASSERT_EQ(second.matches.front().name, std::string("cargo-rust"));
    KAP_ASSERT_EQ(second.matches.front().matched_files.size(), static_cast<std::size_t>(1));

    std::filesystem::remove_all(root);
});

KAP_TEST("--refresh ignores the cache but leaves a fresh one behind")
{
    const std::filesystem::path root = scratch_root("cache-refresh");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    (void) kap::detect::resolve(root, plugins, {});

    kap::detect::Options options;
    options.read_cache                      = false;
    const kap::detect::Resolution refreshed = kap::detect::resolve(root, plugins, options);
    KAP_ASSERT(!refreshed.from_cache);
    KAP_ASSERT(refreshed.matched());

    KAP_ASSERT(kap::detect::resolve(root, plugins, {}).from_cache);

    std::filesystem::remove_all(root);
});

KAP_TEST("editing a marker file invalidates the cache")
{
    // This is the invalidation §3.2 names literally: the cache is keyed on the
    // matched marker files' mtimes. A `file_contains` rule's answer can change
    // with no other visible change on disk, so the marker mtime is the only
    // signal available.
    const std::filesystem::path root = scratch_root("cache-marker");
    write_file(root / "package.json", R"({ "workspaces": [] })");

    const std::vector<kap::plugin::Located> plugins = {make_plugin(
        root,
        "node",
        simple_plugin(
            "node", 20, R"(  file_contains { path: "package.json", pattern: "workspaces" })"))};

    KAP_ASSERT(kap::detect::resolve(root, plugins, {}).matched());
    KAP_ASSERT(kap::detect::resolve(root, plugins, {}).from_cache);

    write_file(root / "package.json", R"({ "name": "x" })");
    bump_mtime(root / "package.json");

    const kap::detect::Resolution after = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(!after.from_cache);
    KAP_ASSERT(!after.matched());

    std::filesystem::remove_all(root);
});

KAP_TEST("deleting a marker file invalidates the cache")
{
    const std::filesystem::path root = scratch_root("cache-delete");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    KAP_ASSERT(kap::detect::resolve(root, plugins, {}).matched());
    std::filesystem::remove(root / "Cargo.toml");

    const kap::detect::Resolution after = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(!after.from_cache);
    KAP_ASSERT(!after.matched());

    std::filesystem::remove_all(root);
});

KAP_TEST("adding a new marker file invalidates the cache")
{
    // The marker-mtime guard alone cannot see this: before the change there
    // were no markers to record. The precheck key hashes the root's directory
    // listing, and a new name in that listing is what invalidates the entry —
    // no mtime is touched here deliberately, to prove the listing guard works
    // on its own.
    const std::filesystem::path             root    = scratch_root("cache-add");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    KAP_ASSERT(!kap::detect::resolve(root, plugins, {}).matched());

    write_file(root / "Cargo.toml", "[package]\n");

    const kap::detect::Resolution after = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(!after.from_cache);
    KAP_ASSERT(after.matched());

    std::filesystem::remove_all(root);
});

KAP_TEST("editing a plugin invalidates the cache")
{
    const std::filesystem::path root = scratch_root("cache-plugin-edit");
    write_file(root / "marker.txt", "x");
    std::vector<kap::plugin::Located> plugins = {
        make_plugin(root, "alpha", simple_plugin("alpha", 10, R"(  file_exists "marker.txt")")),
    };

    KAP_ASSERT_EQ(kap::detect::resolve(root, plugins, {}).matches.front().priority, 10);

    write_file(plugins.front().manifest,
               simple_plugin("alpha", 77, R"(  file_exists "marker.txt")"));
    bump_mtime(plugins.front().manifest);

    const kap::detect::Resolution after = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(!after.from_cache);
    KAP_ASSERT_EQ(after.matches.front().priority, 77);

    std::filesystem::remove_all(root);
});

KAP_TEST("installing another plugin invalidates the cache")
{
    const std::filesystem::path root = scratch_root("cache-plugin-add");
    write_file(root / "marker.txt", "x");

    std::vector<kap::plugin::Located> plugins = {
        make_plugin(root, "alpha", simple_plugin("alpha", 10, R"(  file_exists "marker.txt")")),
    };
    KAP_ASSERT_EQ(kap::detect::resolve(root, plugins, {}).matches.front().name,
                  std::string("alpha"));

    plugins.push_back(
        make_plugin(root, "beta", simple_plugin("beta", 90, R"(  file_exists "marker.txt")")));

    const kap::detect::Resolution after = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(!after.from_cache);
    KAP_ASSERT_EQ(after.matches.front().name, std::string("beta"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a corrupt cache is a miss, never a failure")
{
    const std::filesystem::path root = scratch_root("cache-corrupt");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    (void) kap::detect::resolve(root, plugins, {});
    write_file(kap::detect::cache_file(root), "{ this is not json");

    const kap::detect::Resolution after = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(!after.from_cache);
    KAP_ASSERT(after.matched());

    std::filesystem::remove_all(root);
});

KAP_TEST("invalidate_cache removes the entry and tolerates a missing one")
{
    const std::filesystem::path root = scratch_root("cache-invalidate");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    (void) kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(kap::fs::is_file(kap::detect::cache_file(root)));

    kap::detect::invalidate_cache(root);
    KAP_ASSERT(!kap::fs::exists(kap::detect::cache_file(root)));
    kap::detect::invalidate_cache(root); // second call is a no-op, not an error

    std::filesystem::remove_all(root);
});

KAP_TEST("the cache directory ignores itself so it never lands in a commit")
{
    const std::filesystem::path root = scratch_root("cache-gitignore");
    write_file(root / "Cargo.toml", "[package]\n");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    (void) kap::detect::resolve(root, plugins, {});
    const std::filesystem::path ignore = root / ".kap" / ".gitignore";
    KAP_ASSERT(kap::fs::is_file(ignore));
    // Project-local plugins live in .kap/plugins and must stay committable.
    KAP_ASSERT(kap::fs::read_text(ignore).find("!plugins/") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("a non-matching directory is cached too, so repeat runs stay cheap")
{
    const std::filesystem::path             root    = scratch_root("cache-negative");
    const std::vector<kap::plugin::Located> plugins = {
        make_plugin(
            root, "cargo-rust", simple_plugin("cargo-rust", 40, R"(  file_exists "Cargo.toml")")),
    };

    KAP_ASSERT(!kap::detect::resolve(root, plugins, {}).matched());
    const kap::detect::Resolution second = kap::detect::resolve(root, plugins, {});
    KAP_ASSERT(second.from_cache);
    KAP_ASSERT(!second.matched());

    std::filesystem::remove_all(root);
});
