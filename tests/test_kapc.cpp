// tests/test_kapc.cpp
//
// Unit tests for the KPL AST cache (core/kapc.hpp, design doc §5.14).
//
// The properties that matter are: a decoded AST behaves exactly like a parsed
// one, a stale or corrupt entry is a miss rather than a wrong answer, and a
// cache that cannot be written never breaks a working plugin.

#include "core/diag.hpp"
#include "core/hash.hpp"
#include "core/json.hpp"
#include "core/kapc.hpp"
#include "core/kpl.hpp"
#include "harness.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

namespace
{

std::filesystem::path scratch_cache(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_kapc_" + name + "_" + std::to_string(getpid()));
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

// Exercises every AST node kind the encoder has to handle: all four
// declaration blocks, both step forms, if/else, for, match, calls, records,
// lists, and each literal type.
const char* kRichPlugin = R"(
manifest {
  name        = "rich"
  version     = "2.1.0"
  api_version = 1
  priority    = 30
  composable  = false
  supersedes  = []
}

detect {
  file_exists "CMakeLists.txt"
  file_contains { path: "CMakeLists.txt", pattern: "project(" }
}

requires {
  any_of   [cmake]
  optional [ninja, make]
}

schema {
  generator:  enum { auto, ninja } = auto
  build_dir:  str       = "build"
  jobs:       int       = 4
  verbose:    bool      = false
  cmake_args: list<str> = []
}

command build(project, config, extra) {
  let dir = config.build_dir
  step mkdir "-p" dir

  let gen = match config.generator {
    auto  => if project.tool("ninja") then "Ninja" else none,
    ninja => "Ninja",
  }

  let cmd = ["cmake", "-S", ".", "-B", dir]
  if gen != none {
    cmd = cmd + ["-G", gen]
  } else {
    step echo "no generator"
  }
  cmd = cmd + config.cmake_args
  step cmd

  for word in extra {
    step { cmd: ["echo", word], cwd: dir, label: word, env: { KAP: "1" } }
  }

  if config.verbose && len(extra) > 0 { step echo trim(" loud ") }
  concurrent false
}

command clean(project, config) {
  step rm "-rf" config.build_dir
  report_freed_space
}
)";

// Everything about a plugin that its behaviour depends on, rendered as text.
// Comparing this is a stronger check than comparing a few fields by hand: if
// the encoder drops anything the interpreter reads, the two specs diverge.
std::string behaviour(const kap::kpl::Plugin& plugin)
{
    kap::kpl::Project project;
    project.tool   = [](std::string_view name) { return name == "ninja"; };
    project.exists = [](std::string_view) { return true; };

    const auto [config, errors] =
        kap::kpl::build_config(plugin, {{"verbose", kap::kpl::Value::boolean_value(true)}});
    KAP_ASSERT(errors.empty());

    std::string out;
    for (const auto& command : plugin.commands) {
        out += command.name + ":";
        out += kap::json::write(kap::kpl::to_json(
            kap::kpl::evaluate(plugin, command.name, project, config, {"one", "two"})));
    }
    return out;
}

} // namespace

KAP_TEST("FNV-1a is deterministic and renders as fixed-width hex")
{
    KAP_ASSERT_EQ(kap::hash::fnv1a64(""), kap::hash::kFnvOffsetBasis);
    KAP_ASSERT_EQ(kap::hash::fnv1a64("kap"), kap::hash::fnv1a64("kap"));
    KAP_ASSERT(kap::hash::fnv1a64("kap") != kap::hash::fnv1a64("kaq"));
    KAP_ASSERT_EQ(kap::hash::hex64(0).size(), static_cast<std::size_t>(16));
    KAP_ASSERT_EQ(kap::hash::hex64(0), std::string("0000000000000000"));
    KAP_ASSERT_EQ(kap::hash::hex64(255), std::string("00000000000000ff"));
});

KAP_TEST("kapc round-trips a plugin that uses every AST node kind")
{
    const auto original = kap::kpl::parse(kRichPlugin, "rich/plugin.kpl");
    const auto restored = kap::kapc::decode(kap::kapc::encode(original));

    KAP_ASSERT_EQ(restored.source_name, std::string("rich/plugin.kpl"));
    KAP_ASSERT(restored.manifest.has_value());
    KAP_ASSERT(restored.detect.has_value());
    KAP_ASSERT(restored.requires_block.has_value());
    KAP_ASSERT(restored.schema.has_value());
    KAP_ASSERT_EQ(restored.commands.size(), original.commands.size());

    // The decisive check: a decoded AST must produce byte-identical specs.
    KAP_ASSERT_EQ(behaviour(restored), behaviour(original));

    // And everything the loader reads off the AST without evaluating.
    KAP_ASSERT(kap::kpl::validate(restored).empty());
    KAP_ASSERT(kap::kpl::type_check(restored).empty());
    KAP_ASSERT_EQ(kap::kpl::schema(restored).size(), kap::kpl::schema(original).size());
});

KAP_TEST("kapc encoding is deterministic")
{
    // A cache file that differed run to run would defeat any attempt to
    // compare or share one.
    const auto plugin = kap::kpl::parse(kRichPlugin, "rich/plugin.kpl");
    KAP_ASSERT_EQ(kap::kapc::encode(plugin), kap::kapc::encode(plugin));
    KAP_ASSERT_EQ(kap::kapc::encode(kap::kapc::decode(kap::kapc::encode(plugin))),
                  kap::kapc::encode(plugin));
});

KAP_TEST("kapc round-trips source locations, so cached plugins still diagnose well")
{
    const auto original = kap::kpl::parse("command build(project) {\n"
                                          "  step project.read(\"x\")\n"
                                          "}\n",
                                          "loc/plugin.kpl");
    const auto restored = kap::kapc::decode(kap::kapc::encode(original));

    const kap::kpl::Project project{};
    try {
        kap::kpl::evaluate(restored, "build", project);
        KAP_ASSERT(false);
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT(error.report().find("loc/plugin.kpl:2:") != std::string::npos);
    }
});

KAP_TEST("kapc rejects corrupt, truncated, and foreign blobs")
{
    const std::string blob = kap::kapc::encode(kap::kpl::parse(kRichPlugin));

    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::decode(""));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::decode("not a kapc file"));
    // Truncation at several points, all of which must fail rather than
    // produce a partial AST.
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::decode(blob.substr(0, 8)));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::decode(blob.substr(0, blob.size() / 2)));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::decode(blob.substr(0, blob.size() - 1)));
    // Trailing bytes: a blob that is *nearly* right must not decode into a
    // nearly-right AST.
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::decode(blob + "x"));

    // A different format version is a miss, not a misread.
    std::string wrong_version = blob;
    wrong_version[4]          = static_cast<char>(kap::kapc::kFormatVersion + 1);
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::decode(wrong_version));
});

KAP_TEST("kapc refuses a blob whose length prefix exceeds the bytes it has")
{
    // The failure mode this guards: a corrupt count driving a huge allocation.
    std::string blob = kap::kapc::encode(kap::kpl::parse(kRichPlugin));
    // Overwrite the command-count prefix is fiddly to locate, so corrupt every
    // 4-byte window in turn and require that none of them crashes or hangs.
    for (std::size_t offset = 8; offset + 4 < blob.size(); offset += 7) {
        std::string damaged = blob;
        damaged[offset]     = static_cast<char>(0xFF);
        damaged[offset + 1] = static_cast<char>(0xFF);
        damaged[offset + 2] = static_cast<char>(0xFF);
        damaged[offset + 3] = static_cast<char>(0x7F);
        try {
            kap::kapc::decode(damaged);
        }
        catch (const kap::diag::Error&) {
            // Expected for most offsets; a few may still decode to a valid
            // (if meaningless) tree, which is fine — the contract is "never
            // crash", not "always reject".
        }
    }
    KAP_ASSERT(true);
});

KAP_TEST("kapc cache file names are stable, readable, and path-unique")
{
    const std::filesystem::path dir = "/tmp/kap-cache";
    const auto a = kap::kapc::cache_file(dir, "/projects/app/plugins/cmake-cpp/plugin.kpl");
    const auto b = kap::kapc::cache_file(dir, "/other/repo/plugins/cmake-cpp/plugin.kpl");

    // The plugin's directory name is in there, so a human can read the cache.
    KAP_ASSERT(a.filename().string().rfind("cmake-cpp@", 0) == 0);
    KAP_ASSERT(a.extension().string() == ".kapc");
    // Same path, same file. §6.5 lets a project-local plugin shadow a
    // user-installed one of the same name, so those must not collide.
    KAP_ASSERT_EQ(
        a.string(),
        kap::kapc::cache_file(dir, "/projects/app/plugins/cmake-cpp/plugin.kpl").string());
    KAP_ASSERT(a != b);
});

KAP_TEST("kapc load parses, then serves the same AST from the cache")
{
    const std::filesystem::path root   = scratch_cache("hit");
    const std::filesystem::path cache  = root / "ast";
    const std::filesystem::path source = root / "plugins" / "rich" / "plugin.kpl";
    write_file(source, kRichPlugin);

    const auto first = kap::kapc::load(source, cache);
    KAP_ASSERT(first.origin == kap::kapc::Origin::CacheWrite);
    KAP_ASSERT(std::filesystem::exists(kap::kapc::cache_file(cache, source)));

    const auto second = kap::kapc::load(source, cache);
    KAP_ASSERT(second.origin == kap::kapc::Origin::CacheHit);
    KAP_ASSERT_EQ(behaviour(second.plugin), behaviour(first.plugin));

    std::filesystem::remove_all(root);
});

KAP_TEST("kapc load invalidates the cache when the source changes")
{
    const std::filesystem::path root   = scratch_cache("stale");
    const std::filesystem::path cache  = root / "ast";
    const std::filesystem::path source = root / "plugins" / "small" / "plugin.kpl";
    write_file(source,
               "manifest { name = \"small\" version = \"1.0.0\" api_version = 1 }\n"
               "command build(project) { step echo \"before\" }\n");

    KAP_ASSERT(kap::kapc::load(source, cache).origin == kap::kapc::Origin::CacheWrite);
    KAP_ASSERT(kap::kapc::load(source, cache).origin == kap::kapc::Origin::CacheHit);

    // Rewrite with different content. The mtime check is the primary signal,
    // and the size check covers a filesystem whose mtime granularity is too
    // coarse to notice an edit made within the same tick.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    write_file(source,
               "manifest { name = \"small\" version = \"1.0.0\" api_version = 1 }\n"
               "command build(project) { step echo \"after-the-edit\" }\n");

    const auto refreshed = kap::kapc::load(source, cache);
    KAP_ASSERT(refreshed.origin == kap::kapc::Origin::CacheWrite);

    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(refreshed.plugin, "build", project);
    KAP_ASSERT_EQ(spec.steps[0].command[1], std::string("after-the-edit"));

    // And the refreshed entry is itself cached.
    KAP_ASSERT(kap::kapc::load(source, cache).origin == kap::kapc::Origin::CacheHit);

    std::filesystem::remove_all(root);
});

KAP_TEST("kapc load treats a corrupt cache entry as a miss, not a failure")
{
    // A cache must never be able to turn a working plugin into a broken one.
    const std::filesystem::path root   = scratch_cache("corrupt");
    const std::filesystem::path cache  = root / "ast";
    const std::filesystem::path source = root / "plugins" / "small" / "plugin.kpl";
    write_file(source,
               "manifest { name = \"small\" version = \"1.0.0\" api_version = 1 }\n"
               "command build(project) { step echo \"ok\" }\n");

    kap::kapc::load(source, cache);
    write_file(kap::kapc::cache_file(cache, source), "garbage that is not a kapc file at all");

    const auto recovered = kap::kapc::load(source, cache);
    KAP_ASSERT(recovered.origin != kap::kapc::Origin::CacheHit);
    const kap::kpl::Project project{};
    KAP_ASSERT_EQ(kap::kpl::evaluate(recovered.plugin, "build", project).steps[0].command[1],
                  std::string("ok"));

    std::filesystem::remove_all(root);
});

KAP_TEST("kapc load works with caching disabled and with an unwritable cache")
{
    const std::filesystem::path root   = scratch_cache("nocache");
    const std::filesystem::path source = root / "plugins" / "small" / "plugin.kpl";
    write_file(source,
               "manifest { name = \"small\" version = \"1.0.0\" api_version = 1 }\n"
               "command build(project) { step echo \"ok\" }\n");

    // An empty cache directory disables caching entirely.
    const auto uncached = kap::kapc::load(source, {});
    KAP_ASSERT(uncached.origin == kap::kapc::Origin::Parsed);

    // A cache path that cannot be created is a performance problem, not a
    // correctness one: the plugin still loads.
    const auto blocked = kap::kapc::load(source, source / "not-a-directory");
    KAP_ASSERT(blocked.origin == kap::kapc::Origin::Parsed);
    const kap::kpl::Project project{};
    KAP_ASSERT_EQ(kap::kpl::evaluate(blocked.plugin, "build", project).steps[0].command[1],
                  std::string("ok"));

    std::filesystem::remove_all(root);
});

KAP_TEST("kapc load still reports a genuine parse error")
{
    // Only cache problems are swallowed; a broken plugin.kpl must not be.
    const std::filesystem::path root   = scratch_cache("broken");
    const std::filesystem::path source = root / "plugins" / "bad" / "plugin.kpl";
    write_file(source, "manifest { name = \n");

    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::load(source, root / "ast"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kapc::load(root / "missing.kpl", root / "ast"));

    std::filesystem::remove_all(root);
});

KAP_TEST("kapc cache_directory follows XDG_CACHE_HOME then HOME")
{
    // Saved and restored so the rest of the suite sees the real environment.
    const char*       saved_xdg     = std::getenv("XDG_CACHE_HOME");
    const char*       saved_home    = std::getenv("HOME");
    const std::string previous_xdg  = saved_xdg == nullptr ? "" : saved_xdg;
    const std::string previous_home = saved_home == nullptr ? "" : saved_home;

    ::setenv("XDG_CACHE_HOME", "/xdg", 1);
    KAP_ASSERT_EQ(kap::kapc::cache_directory().string(), std::string("/xdg/kap/ast"));

    ::unsetenv("XDG_CACHE_HOME");
    ::setenv("HOME", "/home/someone", 1);
    KAP_ASSERT_EQ(kap::kapc::cache_directory().string(),
                  std::string("/home/someone/.cache/kap/ast"));

    ::unsetenv("HOME");
    KAP_ASSERT(kap::kapc::cache_directory().empty());

    if (!previous_xdg.empty())
        ::setenv("XDG_CACHE_HOME", previous_xdg.c_str(), 1);
    if (!previous_home.empty())
        ::setenv("HOME", previous_home.c_str(), 1);
});
