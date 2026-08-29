// tests/test_registry.cpp
//
// Unit tests for the plugin manager (core/registry.hpp, design doc §6 and
// Milestone 7).
//
// The install pipeline touches the filesystem and, for the git paths, spawns
// `git`. Those are tested against a real repository created in a scratch
// directory and cloned over a file:// URL, so the tests exercise the same code
// path a network install would without needing a network. Tests that need git
// check for it first and skip cleanly, because a machine without git is not a
// kap bug.
//
// Every test relocates $XDG_DATA_HOME and $XDG_CACHE_HOME, so a plugin the
// developer has actually installed can never be touched or observed.

#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/registry.hpp"
#include "core/sha256.hpp"
#include "core/toml.hpp"
#include "harness.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace
{

std::filesystem::path scratch_root(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_registry_" + name + "_" + std::to_string(getpid()));
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

// Relocate every XDG directory kap writes to, for the lifetime of one test.
class ScopedHome
{
public:
    explicit ScopedHome(const std::filesystem::path& base)
    {
        save("XDG_DATA_HOME");
        save("XDG_CACHE_HOME");
        save("XDG_CONFIG_HOME");
        ::setenv("XDG_DATA_HOME", (base / "data").c_str(), 1);
        ::setenv("XDG_CACHE_HOME", (base / "cache").c_str(), 1);
        ::setenv("XDG_CONFIG_HOME", (base / "config").c_str(), 1);
    }

    ~ScopedHome()
    {
        for (const auto& [name, value] : saved_) {
            if (value.has_value())
                ::setenv(name.c_str(), value->c_str(), 1);
            else
                ::unsetenv(name.c_str());
        }
    }

    ScopedHome(const ScopedHome&)            = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

private:
    void save(const char* name)
    {
        const char* existing = std::getenv(name);
        saved_.emplace_back(
            name, existing != nullptr ? std::optional<std::string>(existing) : std::nullopt);
    }

    std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
};

bool have_git()
{
    // The same PATH scan project.tool() uses, rather than spawning anything.
    const char* path = std::getenv("PATH");
    if (path == nullptr)
        return false;
    std::string            entries(path);
    std::string::size_type start = 0;
    while (start <= entries.size()) {
        const auto        colon = entries.find(':', start);
        const std::string dir =
            entries.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty() && ::access((std::filesystem::path(dir) / "git").c_str(), X_OK) == 0)
            return true;
        if (colon == std::string::npos)
            break;
        start = colon + 1;
    }
    return false;
}

// A minimal but valid plugin, as a real one would be written.
std::string plugin_source(const std::string& name, const std::string& version)
{
    return "manifest {\n  name = \"" + name + "\"\n  version = \"" + version +
           "\"\n  api_version = 1\n  priority = 10\n}\n"
           "detect {\n  file_exists \"marker\"\n}\n"
           "schema {\n  target: str = \"all\"\n}\n"
           "command build(project, config, extra) {\n  step [\"make\", config.target] + extra\n}\n";
}

// Write a plugin directory and return its path.
std::filesystem::path make_plugin_dir(const std::filesystem::path& at,
                                      const std::string&           name,
                                      const std::string&           version = "1.0.0")
{
    write_file(at / "plugin.kpl", plugin_source(name, version));
    write_file(at / "README.md", "# " + name + "\n");
    return at;
}

auto always_yes = [](const std::string&) { return true; };

} // namespace

// --- the index (§6.2) ------------------------------------------------------------------

KAP_TEST("an index parses plugins and bundles")
{
    const std::string          text = R"(
[registry]
version = 1

[plugins.cmake-cpp]
description = "CMake projects"
version = "1.2.0"
url = "https://example.invalid/kap-plugins"
ref = "v1.2.0"
subdir = "cmake-cpp"
checksum = "sha256:abc"
tags = ["cpp", "cmake"]

[bundles.core]
description = "the basics"
plugins = ["cmake-cpp"]
)";
    const kap::registry::Index index =
        kap::registry::parse_index(kap::toml::parse(text, "index.toml"), "index.toml");

    const kap::registry::Entry* entry = index.find("cmake-cpp");
    KAP_ASSERT(entry != nullptr);
    KAP_ASSERT_EQ(entry->version, std::string("1.2.0"));
    KAP_ASSERT_EQ(entry->ref, std::string("v1.2.0"));
    KAP_ASSERT_EQ(entry->subdir, std::string("cmake-cpp"));
    KAP_ASSERT_EQ(entry->checksum, std::string("sha256:abc"));
    KAP_ASSERT_EQ(entry->tags.size(), static_cast<std::size_t>(2));

    const kap::registry::Bundle* bundle = index.find_bundle("core");
    KAP_ASSERT(bundle != nullptr);
    KAP_ASSERT_EQ(bundle->plugins.size(), static_cast<std::size_t>(1));

    KAP_ASSERT(index.find("nothing-here") == nullptr);
    KAP_ASSERT(index.find_bundle("nothing-here") == nullptr);
});

KAP_TEST("an index entry with no url is refused rather than half-understood")
{
    // An entry kap cannot install from would fail later with a confusing
    // message; failing at parse time names the entry.
    const std::string text = "[plugins.broken]\nversion = \"1.0.0\"\n";
    KAP_ASSERT_THROWS(
        kap::diag::Error,
        kap::registry::parse_index(kap::toml::parse(text, "index.toml"), "index.toml"));
});

KAP_TEST("the repository's own registry index parses and lists the bundled plugins")
{
    // Dogfooding: registry/index.toml is a real file this project ships, and a
    // typo in it should fail here rather than the first time someone installs.
    const std::filesystem::path repo_index =
        std::filesystem::path(__FILE__).parent_path().parent_path() / "registry" / "index.toml";
    if (!kap::fs::is_file(repo_index))
        return; // a partial checkout; nothing to assert

    const kap::registry::Index index = kap::registry::parse_index(
        kap::toml::parse(kap::fs::read_text(repo_index), repo_index.string()), repo_index);
    KAP_ASSERT(index.find("cmake-cpp") != nullptr);
    KAP_ASSERT(index.find("cargo-rust") != nullptr);

    // Every plugin a bundle names must actually exist in the index, or
    // `kap plugin install --bundle core` fails halfway through.
    for (const auto& [bundle_name, bundle] : index.bundles) {
        for (const std::string& plugin_name : bundle.plugins) {
            if (index.find(plugin_name) == nullptr) {
                ::kap_test::fail_test(__FILE__,
                                      __LINE__,
                                      "bundle '" + bundle_name + "' names '" + plugin_name +
                                          "', which is not in the index");
            }
        }
    }
});

// --- the lockfile (§6.4) ----------------------------------------------------------------

KAP_TEST("a lockfile round-trips through save and load")
{
    const std::filesystem::path root = scratch_root("lockfile");

    kap::registry::Lockfile lock;
    lock.file = root / "installed-plugins.toml";

    kap::registry::Installed entry;
    entry.name      = "cmake-cpp";
    entry.version   = "1.2.0";
    entry.origin    = kap::registry::Origin::Git;
    entry.url       = "https://example.invalid/x";
    entry.ref       = "main";
    entry.subdir    = "cmake-cpp";
    entry.checksum  = "sha256:deadbeef";
    entry.enabled   = false;
    entry.pinned    = "1.2.0";
    entry.directory = "/somewhere/cmake-cpp";
    lock.plugins.emplace("cmake-cpp", entry);

    kap::registry::save_lockfile(lock);

    const kap::registry::Lockfile reloaded = kap::registry::load_lockfile(lock.file);
    KAP_ASSERT_EQ(reloaded.plugins.size(), static_cast<std::size_t>(1));
    const kap::registry::Installed& back = reloaded.plugins.at("cmake-cpp");
    KAP_ASSERT_EQ(back.version, std::string("1.2.0"));
    KAP_ASSERT_EQ(back.url, std::string("https://example.invalid/x"));
    KAP_ASSERT_EQ(back.checksum, std::string("sha256:deadbeef"));
    KAP_ASSERT(!back.enabled);
    KAP_ASSERT_EQ(back.pinned, std::string("1.2.0"));
    KAP_ASSERT_EQ(back.origin, kap::registry::Origin::Git);

    std::filesystem::remove_all(root);
});

KAP_TEST("a corrupt lockfile loads as empty rather than breaking every command")
{
    // It is derived bookkeeping that install/remove rewrite anyway. Refusing to
    // run `kap build` because it got truncated would be a poor trade: the
    // plugins are still on disk and still discoverable.
    const std::filesystem::path root = scratch_root("lockfile-corrupt");
    write_file(root / "installed-plugins.toml", "this = = not toml\n");
    KAP_ASSERT(kap::registry::load_lockfile(root / "installed-plugins.toml").plugins.empty());
    std::filesystem::remove_all(root);
});

KAP_TEST("apply_lockfile switches off the plugins the user disabled")
{
    kap::registry::Lockfile  lock;
    kap::registry::Installed off;
    off.name    = "sleepy";
    off.enabled = false;
    lock.plugins.emplace("sleepy", off);

    std::vector<kap::plugin::Located> plugins(2);
    plugins[0].name = "sleepy";
    plugins[1].name = "awake";

    kap::registry::apply_lockfile(lock, plugins);
    KAP_ASSERT(!plugins[0].enabled);
    KAP_ASSERT(plugins[1].enabled); // no row means enabled, the sane default
});

// --- payload validation (§6.3 step 3) ------------------------------------------------------

KAP_TEST("a healthy plugin validates clean")
{
    const std::filesystem::path root = scratch_root("validate-ok");
    make_plugin_dir(root / "good", "good");
    KAP_ASSERT(kap::registry::validate_payload(root / "good").empty());
    std::filesystem::remove_all(root);
});

KAP_TEST("validation catches every way a payload can be unusable")
{
    const std::filesystem::path root = scratch_root("validate-bad");

    // No plugin.kpl at all.
    std::filesystem::create_directories(root / "empty");
    KAP_ASSERT(!kap::registry::validate_payload(root / "empty").empty());

    // Does not parse.
    write_file(root / "unparsable" / "plugin.kpl", "manifest { name = ");
    KAP_ASSERT(!kap::registry::validate_payload(root / "unparsable").empty());

    // Parses, but references a config key the schema does not declare.
    write_file(root / "untyped" / "plugin.kpl",
               "manifest { name = \"untyped\" version = \"1.0.0\" api_version = 1 }\n"
               "schema { target: str = \"all\" }\n"
               "command build(project, config, extra) { step [\"make\", config.nope] }\n");
    KAP_ASSERT(!kap::registry::validate_payload(root / "untyped").empty());

    // Parses and type-checks, but its detect rules are malformed — it could
    // never match anything, and only detect::compile knows that.
    write_file(root / "baddetect" / "plugin.kpl",
               "manifest { name = \"baddetect\" version = \"1.0.0\" api_version = 1 }\n"
               "detect { file_smells \"x\" }\n"
               "command build(project, config, extra) { step [\"make\"] }\n");
    KAP_ASSERT(!kap::registry::validate_payload(root / "baddetect").empty());

    // Declares an api_version this kap does not support (§12 Q2: a hard error
    // where the user named the plugin explicitly).
    write_file(root / "future" / "plugin.kpl",
               "manifest { name = \"future\" version = \"1.0.0\" api_version = 99 }\n"
               "command build(project, config, extra) { step [\"make\"] }\n");
    KAP_ASSERT(!kap::registry::validate_payload(root / "future").empty());

    std::filesystem::remove_all(root);
});

// --- installing (§6.3) ----------------------------------------------------------------------

KAP_TEST("installing from a local path copies the plugin and records it")
{
    const std::filesystem::path root = scratch_root("install-local");
    ScopedHome                  home(root / "home");
    make_plugin_dir(root / "src", "localplug");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;
    request.assume_yes   = true;

    const kap::registry::InstallResult result =
        kap::registry::install(request, lock, std::nullopt, always_yes, false);

    KAP_ASSERT(result.installed);
    // The name comes from the *manifest*, not the directory it happened to be
    // in: a git URL's last path component is only a guess.
    KAP_ASSERT_EQ(result.name, std::string("localplug"));
    KAP_ASSERT(kap::fs::is_file(result.directory / "plugin.kpl"));
    KAP_ASSERT_EQ(lock.plugins.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(lock.plugins.at("localplug").version, std::string("1.0.0"));

    std::filesystem::remove_all(root);
});

KAP_TEST("--link installs a symlink so edits to the working copy are live")
{
    const std::filesystem::path root = scratch_root("install-link");
    ScopedHome                  home(root / "home");
    make_plugin_dir(root / "src", "linkplug");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;
    request.assume_yes   = true;
    request.link         = true;

    const kap::registry::InstallResult result =
        kap::registry::install(request, lock, std::nullopt, always_yes, false);
    KAP_ASSERT(result.installed);
    KAP_ASSERT(std::filesystem::is_symlink(result.directory));

    // An edit to the source is visible through the installed path immediately.
    write_file(root / "src" / "plugin.kpl", plugin_source("linkplug", "2.0.0"));
    KAP_ASSERT(kap::fs::read_text(result.directory / "plugin.kpl").find("2.0.0") !=
               std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("removing a linked plugin leaves the working copy alone")
{
    // Deleting the directory a developer is working in would be a catastrophe,
    // and `remove_all` on a symlink follows it on some implementations if you
    // are not careful, so this is checked explicitly.
    const std::filesystem::path root = scratch_root("remove-link");
    ScopedHome                  home(root / "home");
    make_plugin_dir(root / "src", "linkplug");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;
    request.assume_yes   = true;
    request.link         = true;
    (void) kap::registry::install(request, lock, std::nullopt, always_yes, false);

    const kap::registry::InstallResult removed =
        kap::registry::remove("linkplug", lock, root, false);
    KAP_ASSERT(removed.installed);
    KAP_ASSERT(lock.plugins.empty());
    KAP_ASSERT(kap::fs::is_file(root / "src" / "plugin.kpl"));

    std::filesystem::remove_all(root);
});

KAP_TEST("an unusable payload is refused before anything is written")
{
    const std::filesystem::path root = scratch_root("install-invalid");
    ScopedHome                  home(root / "home");
    write_file(root / "src" / "plugin.kpl", "manifest { name = ");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;
    request.assume_yes   = true;

    const kap::registry::InstallResult result =
        kap::registry::install(request, lock, std::nullopt, always_yes, false);
    KAP_ASSERT(!result.installed);
    KAP_ASSERT(lock.plugins.empty());

    std::filesystem::remove_all(root);
});

KAP_TEST("declining the confirmation installs nothing")
{
    // §7: "kap plugin install prints a summary and requires confirmation
    // unless --yes."
    const std::filesystem::path root = scratch_root("install-declined");
    ScopedHome                  home(root / "home");
    make_plugin_dir(root / "src", "declined");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;

    std::string                        shown;
    const kap::registry::InstallResult result = kap::registry::install(
        request,
        lock,
        std::nullopt,
        [&shown](const std::string& summary) {
            shown = summary;
            return false;
        },
        false);

    KAP_ASSERT(!result.installed);
    KAP_ASSERT(lock.plugins.empty());
    // The summary has to say what would happen, or confirming it is theatre.
    KAP_ASSERT(shown.find("declined") != std::string::npos);
    KAP_ASSERT(shown.find("destination") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("reinstalling preserves a disabled state and a pin")
{
    // Reinstalling must not silently re-enable a plugin the user switched off,
    // or drop a version they pinned.
    const std::filesystem::path root = scratch_root("install-preserve");
    ScopedHome                  home(root / "home");
    make_plugin_dir(root / "src", "keepstate");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;
    request.assume_yes   = true;
    (void) kap::registry::install(request, lock, std::nullopt, always_yes, false);

    lock.plugins.at("keepstate").enabled = false;
    lock.plugins.at("keepstate").pinned  = "1.0.0";

    request.force = true;
    (void) kap::registry::install(request, lock, std::nullopt, always_yes, false);

    KAP_ASSERT(!lock.plugins.at("keepstate").enabled);
    KAP_ASSERT_EQ(lock.plugins.at("keepstate").pinned, std::string("1.0.0"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a checksum mismatch stops the install")
{
    // §12 Q3: the checksum is enforced, not advisory. A payload that does not
    // match the index is either a compromised mirror or a stale index; both
    // want the install to stop.
    const std::filesystem::path root = scratch_root("install-checksum");
    ScopedHome                  home(root / "home");
    if (!have_git())
        return; // the registry path installs by clone
    make_plugin_dir(root / "src", "sumcheck");

    // A git repository to serve over file://.
    const std::string git_setup = "cd " + (root / "src").string() +
                                  " && git init -q && git add -A && "
                                  "git -c user.email=t@t -c user.name=t commit -qm x";
    if (std::system(git_setup.c_str()) != 0)
        return;

    const std::string index_text = "[plugins.sumcheck]\nversion = \"1.0.0\"\nurl = \"file://" +
                                   (root / "src").string() +
                                   "\"\nchecksum = \"sha256:" + std::string(64, '0') + "\"\n";
    const kap::registry::Index index =
        kap::registry::parse_index(kap::toml::parse(index_text, "index.toml"), "index.toml");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = "sumcheck";
    request.project_root = root;
    request.assume_yes   = true;

    const kap::registry::InstallResult result =
        kap::registry::install(request, lock, index, always_yes, false);
    KAP_ASSERT(!result.installed);
    KAP_ASSERT(result.message.find("checksum") != std::string::npos);
    KAP_ASSERT(lock.plugins.empty());

    std::filesystem::remove_all(root);
});

KAP_TEST("installing over a git URL fetches, validates, and records the source")
{
    const std::filesystem::path root = scratch_root("install-git");
    ScopedHome                  home(root / "home");
    if (!have_git())
        return;
    make_plugin_dir(root / "repo", "gitplug");
    const std::string git_setup = "cd " + (root / "repo").string() +
                                  " && git init -q && git add -A && "
                                  "git -c user.email=t@t -c user.name=t commit -qm x";
    if (std::system(git_setup.c_str()) != 0)
        return;

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = "file://" + (root / "repo").string();
    request.project_root = root;
    request.assume_yes   = true;

    const kap::registry::InstallResult result =
        kap::registry::install(request, lock, std::nullopt, always_yes, false);
    KAP_ASSERT(result.installed);
    KAP_ASSERT_EQ(result.origin, kap::registry::Origin::Git);
    KAP_ASSERT_EQ(lock.plugins.at("gitplug").origin, kap::registry::Origin::Git);
    // .git must not be copied into the installed plugin: it is a clone's
    // private bookkeeping and would make the payload unhashable-repeatably.
    KAP_ASSERT(!kap::fs::exists(result.directory / ".git"));

    std::filesystem::remove_all(root);
});

KAP_TEST("update refuses to move a pinned plugin")
{
    const std::filesystem::path root = scratch_root("update-pinned");
    ScopedHome                  home(root / "home");
    make_plugin_dir(root / "src", "pinned");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;
    request.assume_yes   = true;
    (void) kap::registry::install(request, lock, std::nullopt, always_yes, false);

    lock.plugins.at("pinned").pinned = "1.0.0";
    const kap::registry::InstallResult result =
        kap::registry::update("pinned", lock, std::nullopt, root, false);
    KAP_ASSERT(!result.installed);
    KAP_ASSERT(result.message.find("pinned") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("update says a linked plugin is always current instead of re-copying it")
{
    const std::filesystem::path root = scratch_root("update-linked");
    ScopedHome                  home(root / "home");
    make_plugin_dir(root / "src", "linked");

    kap::registry::Lockfile lock;
    lock.file = root / "home" / "lock.toml";

    kap::registry::InstallRequest request;
    request.source       = (root / "src").string();
    request.project_root = root;
    request.assume_yes   = true;
    request.link         = true;
    (void) kap::registry::install(request, lock, std::nullopt, always_yes, false);

    const kap::registry::InstallResult result =
        kap::registry::update("linked", lock, std::nullopt, root, false);
    KAP_ASSERT(!result.installed);
    KAP_ASSERT(result.message.find("linked") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("removing a plugin that is not installed says so")
{
    const std::filesystem::path root = scratch_root("remove-absent");
    kap::registry::Lockfile     lock;
    lock.file = root / "lock.toml";

    const kap::registry::InstallResult result = kap::registry::remove("ghost", lock, root, false);
    KAP_ASSERT(!result.installed);
    KAP_ASSERT(result.message.find("not installed") != std::string::npos);

    std::filesystem::remove_all(root);
});

// --- scaffolding (§6.1 `kap plugin new`) ---------------------------------------------------

KAP_TEST("a scaffolded plugin is immediately valid and its test case passes")
{
    // The scaffold's whole value is that `kap plugin doctor` and `kap plugin
    // test` say something useful the moment it is created. A template that
    // does not validate would send every new plugin author debugging kap's own
    // starting point.
    const std::filesystem::path root = scratch_root("scaffold");

    const kap::registry::InstallResult result =
        kap::registry::scaffold("brand-new", "build-system", root / "brand-new");
    KAP_ASSERT(result.installed);
    KAP_ASSERT(kap::fs::is_file(root / "brand-new" / "plugin.kpl"));
    KAP_ASSERT(kap::fs::is_file(root / "brand-new" / "README.md"));
    KAP_ASSERT(kap::registry::validate_payload(root / "brand-new").empty());

    // The generated manifest really does carry the name it was asked for.
    KAP_ASSERT(
        kap::fs::read_text(root / "brand-new" / "plugin.kpl").find("name        = \"brand-new\"") !=
        std::string::npos);

    // And the fixture case it ships passes.
    kap::plugin::Located located;
    located.name                                     = "brand-new";
    located.directory                                = root / "brand-new";
    located.manifest                                 = located.directory / "plugin.kpl";
    const std::vector<kap::plugin::CaseResult> cases = kap::plugin::run_tests(located, {});
    KAP_ASSERT_EQ(cases.size(), static_cast<std::size_t>(1));
    if (!cases.front().passed)
        ::kap_test::fail_test(
            __FILE__, __LINE__, "scaffolded case failed: " + cases.front().detail);

    std::filesystem::remove_all(root);
});

KAP_TEST("scaffolding refuses an unknown template and an occupied directory")
{
    const std::filesystem::path root = scratch_root("scaffold-refuse");

    const kap::registry::InstallResult bad_template =
        kap::registry::scaffold("x", "no-such-template", root / "x");
    KAP_ASSERT(!bad_template.installed);
    KAP_ASSERT(bad_template.message.find("build-system") != std::string::npos);

    std::filesystem::create_directories(root / "taken");
    const kap::registry::InstallResult occupied =
        kap::registry::scaffold("taken", "build-system", root / "taken");
    KAP_ASSERT(!occupied.installed);
    KAP_ASSERT(occupied.message.find("already exists") != std::string::npos);

    std::filesystem::remove_all(root);
});
