// tests/test_config.cpp
//
// Unit tests for the layered configuration (core/config.hpp, design doc §5.12
// and Milestone 6).
//
// The precedence chain — schema defaults → global → project → --set — is the
// whole promise of §5.12 ("a user can override the CMake generator ... without
// forking the plugin"), so each link in it gets its own test, and so does each
// way a layer can be wrong.
//
// Every test points $XDG_CONFIG_HOME at a scratch directory before calling
// config::load, so a config file the developer happens to have on their own
// machine can never change a result.

#include "core/config.hpp"
#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/kpl.hpp"
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
                                      ("kap_config_" + name + "_" + std::to_string(getpid()));
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

// Point the global configuration at a scratch directory for the lifetime of
// one test, restoring whatever was there before.
//
// Without this, a developer with a real ~/.config/kap/config.toml would see
// different test results from CI — the exact class of flake that makes a suite
// stop being trusted.
class ScopedConfigHome
{
public:
    explicit ScopedConfigHome(std::filesystem::path directory) : directory_(std::move(directory))
    {
        if (const char* existing = std::getenv("XDG_CONFIG_HOME"); existing != nullptr) {
            had_previous_ = true;
            previous_     = existing;
        }
        ::setenv("XDG_CONFIG_HOME", directory_.c_str(), 1);
    }

    ~ScopedConfigHome()
    {
        if (had_previous_)
            ::setenv("XDG_CONFIG_HOME", previous_.c_str(), 1);
        else
            ::unsetenv("XDG_CONFIG_HOME");
    }

    ScopedConfigHome(const ScopedConfigHome&)            = delete;
    ScopedConfigHome& operator=(const ScopedConfigHome&) = delete;

    // ~/.config/kap/config.toml under the scratch directory.
    std::filesystem::path file() const
    {
        return directory_ / "kap" / "config.toml";
    }

private:
    std::filesystem::path directory_;
    std::string           previous_;
    bool                  had_previous_ = false;
};

// A plugin with one field of every schema type, so coercion can be tested
// without depending on a bundled plugin's schema staying put.
const char* kSchemaPlugin = R"(
manifest {
  name        = "demo"
  version     = "1.0.0"
  api_version = 1
  priority    = 10
}
detect { file_exists "marker" }
schema {
  generator:  enum { auto, ninja, make } = auto
  build_dir:  str       = "build"
  jobs:       int       = 1
  release:    bool      = false
  cmake_args: list<str> = []
  levels:     list<int> = []
}
command build(project, config, extra) {
  step ["echo", config.build_dir]
}
)";

kap::kpl::Plugin demo_plugin()
{
    return kap::kpl::parse(kSchemaPlugin, "demo/plugin.kpl");
}

} // namespace

// --- layer loading ------------------------------------------------------------------

KAP_TEST("configuration is optional: no files means no layers and safe defaults")
{
    const std::filesystem::path root = scratch_root("absent");
    ScopedConfigHome            home(root / "xdg");

    const kap::config::Merged merged = kap::config::load(root);
    KAP_ASSERT(merged.layers.empty());
    KAP_ASSERT_EQ(merged.settings.max_walk_up, 0);
    KAP_ASSERT_EQ(merged.settings.ecosystem, std::string(""));
    KAP_ASSERT(merged.settings.hooks.empty());

    std::filesystem::remove_all(root);
});

KAP_TEST("a malformed configuration file is fatal, never silently ignored")
{
    // Acting on a config we could not read would be a lie: the user would see
    // their setting having no effect with nothing to explain why.
    const std::filesystem::path root = scratch_root("malformed");
    ScopedConfigHome            home(root / "xdg");
    write_file(root / "kap.toml", "this is not = = toml\n");

    KAP_ASSERT_THROWS(kap::diag::Error, kap::config::load(root));

    std::filesystem::remove_all(root);
});

KAP_TEST("both layers load, project last so it wins")
{
    const std::filesystem::path root = scratch_root("layers");
    ScopedConfigHome            home(root / "xdg");
    write_file(home.file(), "[detect]\nmax_walk_up = 5\n");
    write_file(root / "kap.toml", "[detect]\nmax_walk_up = 2\n");

    const kap::config::Merged merged = kap::config::load(root);
    KAP_ASSERT_EQ(merged.layers.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(merged.layers[0].name, std::string("global"));
    KAP_ASSERT_EQ(merged.layers[1].name, std::string("project"));
    KAP_ASSERT_EQ(merged.settings.max_walk_up, 2);

    std::filesystem::remove_all(root);
});

KAP_TEST("merging is deep, so a project file need not restate the whole table")
{
    const std::filesystem::path root = scratch_root("deep-merge");
    ScopedConfigHome            home(root / "xdg");
    write_file(home.file(), "[plugins.demo]\nbuild_dir = \"global-dir\"\njobs = 8\n");
    write_file(root / "kap.toml", "[plugins.demo]\nbuild_dir = \"project-dir\"\n");

    const kap::config::Merged merged    = kap::config::load(root);
    const kap::toml::Value    effective = kap::config::effective(merged);

    kap::toml::Document document;
    document.root() = effective;
    // The project overrode one key; the other survives from the global layer.
    KAP_ASSERT_EQ(document.get("plugins.demo.build_dir")->str, std::string("project-dir"));
    KAP_ASSERT_EQ(document.get("plugins.demo.jobs")->integer, static_cast<std::int64_t>(8));

    std::filesystem::remove_all(root);
});

// --- kap's own settings -----------------------------------------------------------

KAP_TEST("detect settings and hooks are read out of the merged table")
{
    const std::filesystem::path root = scratch_root("settings");
    ScopedConfigHome            home(root / "xdg");
    write_file(root / "kap.toml",
               "[detect]\nmax_walk_up = 3\necosystem = \"cargo-rust\"\n"
               "[hooks]\npre_build = \"echo before\"\npost_test = \"echo after\"\n");

    const kap::config::Merged merged = kap::config::load(root);
    KAP_ASSERT_EQ(merged.settings.max_walk_up, 3);
    KAP_ASSERT_EQ(merged.settings.ecosystem, std::string("cargo-rust"));
    KAP_ASSERT_EQ(merged.hook("pre", "build").value_or(""), std::string("echo before"));
    KAP_ASSERT_EQ(merged.hook("post", "test").value_or(""), std::string("echo after"));
    KAP_ASSERT(!merged.hook("pre", "test").has_value());

    std::filesystem::remove_all(root);
});

KAP_TEST("a setting of the wrong type warns and falls back rather than crashing")
{
    const std::filesystem::path root = scratch_root("bad-setting");
    ScopedConfigHome            home(root / "xdg");
    write_file(root / "kap.toml", "[detect]\nmax_walk_up = \"lots\"\necosystem = 7\n");

    const kap::config::Merged merged = kap::config::load(root);
    KAP_ASSERT_EQ(merged.settings.max_walk_up, 0);
    KAP_ASSERT_EQ(merged.settings.ecosystem, std::string(""));
    KAP_ASSERT_EQ(merged.warnings.size(), static_cast<std::size_t>(2));

    std::filesystem::remove_all(root);
});

// --- the plugin config record (§5.7 + §5.12) -----------------------------------------

KAP_TEST("with no configuration a plugin gets exactly its schema defaults")
{
    const std::filesystem::path root = scratch_root("defaults");
    ScopedConfigHome            home(root / "xdg");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(demo_plugin(), "demo", kap::config::load(root), {});

    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT_EQ(config.values.at("build_dir").string, std::string("build"));
    KAP_ASSERT_EQ(config.values.at("generator").string, std::string("auto"));
    KAP_ASSERT_EQ(config.values.at("jobs").integer, static_cast<std::int64_t>(1));
    KAP_ASSERT(!config.values.at("release").boolean);

    std::filesystem::remove_all(root);
});

KAP_TEST("the full precedence chain: defaults, then global, then project, then --set")
{
    // This is §5.12's headline promise in one test.
    const std::filesystem::path root = scratch_root("precedence");
    ScopedConfigHome            home(root / "xdg");
    write_file(home.file(),
               "[plugins.demo]\ngenerator = \"make\"\nbuild_dir = \"from-global\"\njobs = 2\n");
    write_file(root / "kap.toml", "[plugins.demo]\nbuild_dir = \"from-project\"\n");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(demo_plugin(), "demo", kap::config::load(root), {"jobs=16"});

    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT_EQ(config.values.at("release").boolean, false);                // schema default
    KAP_ASSERT_EQ(config.values.at("generator").string, std::string("make")); // global
    KAP_ASSERT_EQ(config.values.at("build_dir").string, std::string("from-project")); // project
    KAP_ASSERT_EQ(config.values.at("jobs").integer, static_cast<std::int64_t>(16));   // --set

    std::filesystem::remove_all(root);
});

KAP_TEST("an unknown key under [plugins.<name>] is an error, not a warning")
{
    // §5.7: "The core validates ... *before* invoking KPL, so typos fail fast
    // with a clear error." A silently ignored `genrator = "ninja"` would leave
    // the user staring at a build that ignores their config for no reason.
    const std::filesystem::path root = scratch_root("unknown-key");
    ScopedConfigHome            home(root / "xdg");
    write_file(root / "kap.toml", "[plugins.demo]\ngenrator = \"ninja\"\n");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(demo_plugin(), "demo", kap::config::load(root), {});
    KAP_ASSERT_EQ(config.errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(config.errors.front().find("genrator") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("an enum rejection names the members that would have worked")
{
    const std::filesystem::path root = scratch_root("enum-error");
    ScopedConfigHome            home(root / "xdg");

    const kap::config::PluginConfig config = kap::config::for_plugin(
        demo_plugin(), "demo", kap::config::load(root), {"generator=clown"});
    KAP_ASSERT_EQ(config.errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(config.errors.front().find("clown") != std::string::npos);
    KAP_ASSERT(config.errors.front().find("ninja") != std::string::npos);

    std::filesystem::remove_all(root);
});

KAP_TEST("another plugin's section is ignored, not treated as unknown keys")
{
    const std::filesystem::path root = scratch_root("other-plugin");
    ScopedConfigHome            home(root / "xdg");
    write_file(root / "kap.toml",
               "[plugins.demo]\nbuild_dir = \"mine\"\n[plugins.other]\nwhatever = 1\n");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(demo_plugin(), "demo", kap::config::load(root), {});
    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT_EQ(config.values.at("build_dir").string, std::string("mine"));

    std::filesystem::remove_all(root);
});

// --- --set coercion ------------------------------------------------------------------

KAP_TEST("--set coerces text to the schema field's declared type")
{
    // The command line has no types; the schema is the only thing that can say
    // whether `release=true` means the string "true" or the boolean true.
    const std::filesystem::path root = scratch_root("coerce");
    ScopedConfigHome            home(root / "xdg");

    const kap::config::PluginConfig config = kap::config::for_plugin(demo_plugin(),
                                                                     "demo",
                                                                     kap::config::load(root),
                                                                     {"release=true",
                                                                      "jobs=12",
                                                                      "build_dir=out",
                                                                      "generator=ninja",
                                                                      "cmake_args=-DA=1,-DB=2",
                                                                      "levels=1,2,3"});

    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT_EQ(config.values.at("release").kind, kap::kpl::Value::Kind::Boolean);
    KAP_ASSERT(config.values.at("release").boolean);
    KAP_ASSERT_EQ(config.values.at("jobs").kind, kap::kpl::Value::Kind::Integer);
    KAP_ASSERT_EQ(config.values.at("jobs").integer, static_cast<std::int64_t>(12));
    KAP_ASSERT_EQ(config.values.at("build_dir").string, std::string("out"));
    KAP_ASSERT_EQ(config.values.at("generator").string, std::string("ninja"));

    // A list-valued key still has to be expressible in one `--set` pair, so
    // commas separate elements. Note "-DA=1" keeps its own '=': only the first
    // one splits key from value.
    const kap::kpl::Value& args = config.values.at("cmake_args");
    KAP_ASSERT_EQ(args.list.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(args.list[0].string, std::string("-DA=1"));
    KAP_ASSERT_EQ(args.list[1].string, std::string("-DB=2"));

    const kap::kpl::Value& levels = config.values.at("levels");
    KAP_ASSERT_EQ(levels.list.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(levels.list[2].integer, static_cast<std::int64_t>(3));

    std::filesystem::remove_all(root);
});

KAP_TEST("--set rejects text that cannot be the declared type")
{
    const std::filesystem::path root = scratch_root("coerce-bad");
    ScopedConfigHome            home(root / "xdg");

    for (const char* assignment : {"release=yes", "jobs=many", "levels=1,two"}) {
        const kap::config::PluginConfig config =
            kap::config::for_plugin(demo_plugin(), "demo", kap::config::load(root), {assignment});
        KAP_ASSERT(!config.errors.empty());
    }

    std::filesystem::remove_all(root);
});

KAP_TEST("--set with an empty list value means an empty list, not one empty string")
{
    const std::filesystem::path root = scratch_root("coerce-empty");
    ScopedConfigHome            home(root / "xdg");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(demo_plugin(), "demo", kap::config::load(root), {"cmake_args="});
    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT(config.values.at("cmake_args").list.empty());

    std::filesystem::remove_all(root);
});

// --- config set (writing) ------------------------------------------------------------

KAP_TEST("set_key creates a file and its intermediate tables")
{
    const std::filesystem::path root = scratch_root("set-create");
    const std::filesystem::path file = root / "nested" / "kap.toml";

    kap::config::set_key(file, "plugins.cmake-cpp.generator", "ninja");

    const kap::toml::Document doc = kap::toml::parse(kap::fs::read_text(file), file.string());
    KAP_ASSERT_EQ(doc.get("plugins.cmake-cpp.generator")->str, std::string("ninja"));

    std::filesystem::remove_all(root);
});

KAP_TEST("set_key preserves the keys it did not touch")
{
    const std::filesystem::path root = scratch_root("set-preserve");
    const std::filesystem::path file = root / "kap.toml";
    write_file(file, "[plugins.demo]\nbuild_dir = \"keepme\"\njobs = 4\n");

    kap::config::set_key(file, "plugins.demo.jobs", "9");

    const kap::toml::Document doc = kap::toml::parse(kap::fs::read_text(file), file.string());
    KAP_ASSERT_EQ(doc.get("plugins.demo.build_dir")->str, std::string("keepme"));
    KAP_ASSERT_EQ(doc.get("plugins.demo.jobs")->integer, static_cast<std::int64_t>(9));

    std::filesystem::remove_all(root);
});

KAP_TEST("set_key infers bool and int from the text, and leaves the rest a string")
{
    // `kap config set detect.max_walk_up 3` obviously means the number: storing
    // "3" would fail the settings validation on a key the user just set right.
    const std::filesystem::path root = scratch_root("set-types");
    const std::filesystem::path file = root / "kap.toml";

    kap::config::set_key(file, "a.flag", "true");
    kap::config::set_key(file, "a.count", "42");
    kap::config::set_key(file, "a.negative", "-7");
    kap::config::set_key(file, "a.text", "ninja");
    kap::config::set_key(file, "a.versionish", "1.2.3");

    const kap::toml::Document doc = kap::toml::parse(kap::fs::read_text(file), file.string());
    KAP_ASSERT_EQ(doc.get("a.flag")->kind, kap::toml::Value::Kind::Boolean);
    KAP_ASSERT_EQ(doc.get("a.count")->integer, static_cast<std::int64_t>(42));
    KAP_ASSERT_EQ(doc.get("a.negative")->integer, static_cast<std::int64_t>(-7));
    KAP_ASSERT_EQ(doc.get("a.text")->str, std::string("ninja"));
    KAP_ASSERT_EQ(doc.get("a.versionish")->str, std::string("1.2.3"));

    std::filesystem::remove_all(root);
});

KAP_TEST("set_key refuses to rewrite a file it cannot parse")
{
    // Rewriting would emit our (empty) understanding of the file and destroy
    // whatever the user had written.
    const std::filesystem::path root = scratch_root("set-unparsable");
    const std::filesystem::path file = root / "kap.toml";
    write_file(file, "= = nonsense\n");

    KAP_ASSERT_THROWS(kap::diag::Error, kap::config::set_key(file, "a.b", "c"));
    KAP_ASSERT_EQ(kap::fs::read_text(file), std::string("= = nonsense\n"));

    std::filesystem::remove_all(root);
});

KAP_TEST("set_key refuses to bury an existing scalar under a new table")
{
    const std::filesystem::path root = scratch_root("set-conflict");
    const std::filesystem::path file = root / "kap.toml";
    write_file(file, "plugins = 3\n");

    KAP_ASSERT_THROWS(kap::diag::Error, kap::config::set_key(file, "plugins.demo.jobs", "1"));

    std::filesystem::remove_all(root);
});

KAP_TEST("a value written by set_key is readable by load")
{
    const std::filesystem::path root = scratch_root("set-roundtrip");
    ScopedConfigHome            home(root / "xdg");

    kap::config::set_key(kap::config::project_file(root), "detect.max_walk_up", "4");
    KAP_ASSERT_EQ(kap::config::load(root).settings.max_walk_up, 4);

    std::filesystem::remove_all(root);
});

// --- core-injected values (Milestone 9) ---------------------------------------------
//
// `kap doctor` ships as a KPL plugin (§4), and the core supplies the one thing
// KPL cannot see for itself: what the *other* matched plugins require. These
// tests pin where that injection sits in §5.12's precedence chain.

namespace
{

// A plugin shaped like the bundled `doctor`: it declares the fields the core
// injects, with empty defaults so it still works standalone.
const char* kInjectablePlugin = R"(
manifest { name = "doctor" version = "1.0.0" api_version = 1 priority = 1 }
detect { dir_exists "." }
schema {
  required_tools: list<str> = []
  strict:         bool      = false
}
command doctor(project, config) { step ["echo", "ok"] }
)";

std::map<std::string, kap::kpl::Value> tool_list(std::vector<std::string> names)
{
    std::vector<kap::kpl::Value> values;
    for (std::string& name : names)
        values.push_back(kap::kpl::Value::string_value(std::move(name)));
    std::map<std::string, kap::kpl::Value> injected;
    injected["required_tools"] = kap::kpl::Value::list_value(std::move(values));
    return injected;
}

} // namespace

KAP_TEST("an injected value reaches the config record")
{
    const std::filesystem::path root = scratch_root("inject");
    ScopedConfigHome            home(root / "xdg");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(kap::kpl::parse(kInjectablePlugin),
                                "doctor",
                                kap::config::load(root),
                                {},
                                tool_list({"cmake", "ninja"}));

    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT_EQ(config.values.at("required_tools").list.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(config.values.at("required_tools").list[0].string, std::string("cmake"));

    std::filesystem::remove_all(root);
});

KAP_TEST("kap.toml overrides an injected value")
{
    // Injection sits at the bottom of §5.12's chain, just above the schema
    // defaults, so "later wins" holds with no exception. The alternative —
    // making the injection unoverridable so a project could not understate what
    // doctor checks — buys nothing: a committed kap.toml already runs arbitrary
    // shell through §5.13's hooks. What it costs is the ability to experiment.
    const std::filesystem::path root = scratch_root("inject-override");
    ScopedConfigHome            home(root / "xdg");
    write_file(root / "kap.toml", "[plugins.doctor]\nrequired_tools = [\"ss,lsof\"]\n");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(kap::kpl::parse(kInjectablePlugin),
                                "doctor",
                                kap::config::load(root),
                                {},
                                tool_list({"cmake"}));

    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT_EQ(config.values.at("required_tools").list.size(), static_cast<std::size_t>(1));
    // A comma-joined `any_of` group is a single string, which is why it can be
    // expressed in a TOML array element but not through --set.
    KAP_ASSERT_EQ(config.values.at("required_tools").list[0].string, std::string("ss,lsof"));

    std::filesystem::remove_all(root);
});

KAP_TEST("--set overrides an injected value")
{
    const std::filesystem::path root = scratch_root("inject-set");
    ScopedConfigHome            home(root / "xdg");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(kap::kpl::parse(kInjectablePlugin),
                                "doctor",
                                kap::config::load(root),
                                {"required_tools=only-this"},
                                tool_list({"cmake", "ninja"}));

    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT_EQ(config.values.at("required_tools").list.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(config.values.at("required_tools").list[0].string, std::string("only-this"));

    std::filesystem::remove_all(root);
});

KAP_TEST("an injected key the schema does not declare is dropped, not reported")
{
    // A plugin that defines `doctor` without declaring `required_tools` is not
    // asking for the injection, and it is certainly not misconfigured. Adding a
    // field its type checker never saw would also be a hole in §5.7's promise
    // that `config` matches the schema.
    const std::filesystem::path root = scratch_root("inject-undeclared");
    ScopedConfigHome            home(root / "xdg");

    const kap::kpl::Plugin plugin = kap::kpl::parse(R"(
manifest { name = "plain" version = "1.0.0" api_version = 1 }
schema { greeting: str = "hi" }
command doctor(project, config) { step ["echo", config.greeting] }
)");

    const kap::config::PluginConfig config =
        kap::config::for_plugin(plugin, "plain", kap::config::load(root), {}, tool_list({"cmake"}));

    KAP_ASSERT(config.errors.empty());
    KAP_ASSERT(config.values.find("required_tools") == config.values.end());
    KAP_ASSERT_EQ(config.values.at("greeting").string, std::string("hi"));

    std::filesystem::remove_all(root);
});
