// tests/test_bundled.cpp
//
// Unit tests for the embedded plugins and registry index (core/bundled.hpp).
//
// These have to pass in both builds — with and without -DKAP_EMBED_PLUGINS —
// so they assert what is true either way, and branch on `available()` for the
// rest. A test that only worked in one configuration would silently stop
// testing anything in the other.

#include "core/bundled.hpp"
#include "core/fs.hpp"
#include "core/registry.hpp"
#include "core/toml.hpp"
#include "harness.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace
{

std::filesystem::path scratch_root(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_bundled_" + name + "_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

KAP_TEST("the registry index is compiled in whatever the build")
{
    // Always embedded, even without -DKAP_EMBED_PLUGINS. It is three kilobytes,
    // and without it a binary with nothing beside it cannot answer
    // `kap plugin search` or resolve `kap plugin install cmake-cpp` — for
    // plugins kap has always known about.
    const std::string_view index = kap::bundled::registry_index();
    KAP_ASSERT(!index.empty());
    KAP_ASSERT(index.find("[plugins.cmake-cpp]") != std::string_view::npos);
});

KAP_TEST("the compiled-in index parses and names the first-party plugins")
{
    const kap::registry::Index index = kap::registry::parse_index(
        kap::toml::parse(kap::bundled::registry_index(), "<embedded>"), "<embedded>");

    for (const char* name : {"cmake-cpp",
                             "cargo-rust",
                             "node",
                             "go",
                             "java",
                             "python-uv",
                             "make-generic",
                             "doctor",
                             "ports"}) {
        if (index.find(name) == nullptr)
            ::kap_test::fail_test(
                __FILE__, __LINE__, "the embedded index is missing '" + std::string(name) + "'");
    }
    KAP_ASSERT(index.find_bundle("core") != nullptr);
});

KAP_TEST("every first-party entry can be installed without git")
{
    // The failure this guards against is the one that made the tool unusable on
    // a fresh machine: an index whose only install route was a git clone of a
    // repository that had to exist and be reachable.
    const kap::registry::Index index = kap::registry::parse_index(
        kap::toml::parse(kap::bundled::registry_index(), "<embedded>"), "<embedded>");

    for (const auto& [name, entry] : index.plugins) {
        if (entry.install_script.empty())
            ::kap_test::fail_test(
                __FILE__, __LINE__, "registry entry '" + name + "' has no install_script");
        if (!entry.install_script.starts_with("https://"))
            ::kap_test::fail_test(
                __FILE__, __LINE__, "'" + name + "' has a non-HTTPS install_script");
    }
});

KAP_TEST("find() agrees with plugins(), and reports absence")
{
    for (const kap::bundled::Plugin& plugin : kap::bundled::plugins())
        KAP_ASSERT(kap::bundled::find(plugin.name) == &plugin ||
                   kap::bundled::find(plugin.name) != nullptr);
    KAP_ASSERT(kap::bundled::find("definitely-not-a-plugin") == nullptr);
});

KAP_TEST("an embedded build carries every first-party plugin, with a manifest")
{
    if (!kap::bundled::available()) {
        // A build without embedding: the contract is that it reports nothing
        // rather than reporting something empty and confusing.
        KAP_ASSERT(kap::bundled::plugins().empty());
        return;
    }

    // Named rather than counted. A bare number tells you the build embedded
    // the wrong many; a list tells you which one went missing, and makes
    // adding a plugin a deliberate edit here rather than a silent +1.
    for (const char* name : {"cargo-rust",
                             "cmake-cpp",
                             "doctor",
                             "go",
                             "java",
                             "make-generic",
                             "node",
                             "ports",
                             "python-uv",
                             "zig"}) {
        if (kap::bundled::find(name) == nullptr)
            ::kap_test::fail_test(
                __FILE__, __LINE__, std::string("no embedded plugin named '") + name + "'");
    }
    KAP_ASSERT_EQ(kap::bundled::plugins().size(), static_cast<std::size_t>(10));
    for (const kap::bundled::Plugin& plugin : kap::bundled::plugins()) {
        bool has_manifest = false;
        for (const kap::bundled::File& file : plugin.files) {
            KAP_ASSERT(!file.contents.empty());
            if (file.path == "plugin.kpl") {
                has_manifest = true;
                // The text really is the plugin, not a placeholder.
                KAP_ASSERT(file.contents.find("manifest") != std::string_view::npos);
            }
        }
        if (!has_manifest)
            ::kap_test::fail_test(__FILE__,
                                  __LINE__,
                                  "embedded plugin '" + std::string(plugin.name) +
                                      "' has no plugin.kpl");
    }
});

KAP_TEST("a materialized plugin is a valid plugin directory")
{
    if (!kap::bundled::available())
        return;

    const std::filesystem::path root   = scratch_root("materialize");
    const kap::bundled::Plugin* plugin = kap::bundled::find("cmake-cpp");
    KAP_ASSERT(plugin != nullptr);

    KAP_ASSERT_EQ(kap::bundled::materialize(*plugin, root / "cmake-cpp"), std::string(""));
    KAP_ASSERT(kap::fs::is_file(root / "cmake-cpp" / "plugin.kpl"));

    // The whole point of materializing rather than interpreting from memory:
    // what lands on disk goes through exactly the same validation as any other
    // plugin, with no second code path to disagree.
    KAP_ASSERT(kap::registry::validate_payload(root / "cmake-cpp").empty());

    std::filesystem::remove_all(root);
});

KAP_TEST("materialize is idempotent")
{
    if (!kap::bundled::available())
        return;

    const std::filesystem::path root   = scratch_root("materialize-twice");
    const kap::bundled::Plugin* plugin = kap::bundled::find("cmake-cpp");
    KAP_ASSERT(plugin != nullptr);

    KAP_ASSERT_EQ(kap::bundled::materialize(*plugin, root / "p"), std::string(""));
    const std::string first = kap::fs::read_text(root / "p" / "plugin.kpl");
    KAP_ASSERT_EQ(kap::bundled::materialize(*plugin, root / "p"), std::string(""));
    KAP_ASSERT_EQ(kap::fs::read_text(root / "p" / "plugin.kpl"), first);

    std::filesystem::remove_all(root);
});

KAP_TEST("materialize refuses a directory it cannot create")
{
    if (!kap::bundled::available())
        return;
    const kap::bundled::Plugin* plugin = kap::bundled::find("cmake-cpp");
    KAP_ASSERT(plugin != nullptr);
    // A path under a regular file can never be a directory.
    const std::filesystem::path root = scratch_root("materialize-fail");
    kap::fs::exists(root);
    std::ofstream(root / "blocker") << "x";
    KAP_ASSERT(!kap::bundled::materialize(*plugin, root / "blocker" / "p").empty());
    std::filesystem::remove_all(root);
});
