// core/plugin.cpp
//
// Implementation of the plugin discovery and fixture-test runner declared in
// core/plugin.hpp.

#include "core/plugin.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "core/bundled.hpp"
#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/json.hpp"
#include "core/kapc.hpp"
#include "core/kpl.hpp"
#include "core/paths.hpp"

namespace kap
{
namespace plugin
{

namespace
{

constexpr std::string_view kExpectedSuffix = ".steps.json";

// Convert one JSON scalar (or array of scalars) into the KPL value a config
// override needs. Anything else is refused: a config key cannot hold a nested
// object, because the schema types in §5.7 have no such shape.
std::optional<kpl::Value> config_value(const json::Value& value)
{
    switch (value.kind) {
        case json::Value::Kind::String:
            return kpl::Value::string_value(value.string);
        case json::Value::Kind::Integer:
            return kpl::Value::integer_value(value.integer);
        case json::Value::Kind::Boolean:
            return kpl::Value::boolean_value(value.boolean);
        case json::Value::Kind::Array:
            {
                std::vector<kpl::Value> items;
                items.reserve(value.array.size());
                for (const json::Value& element : value.array) {
                    if (element.kind == json::Value::Kind::String)
                        items.push_back(kpl::Value::string_value(element.string));
                    else if (element.kind == json::Value::Kind::Integer)
                        items.push_back(kpl::Value::integer_value(element.integer));
                    else
                        return std::nullopt;
                }
                return kpl::Value::list_value(std::move(items));
            }
        default:
            return std::nullopt;
    }
}

std::vector<std::string> string_array(const json::Value* value)
{
    std::vector<std::string> result;
    if (value == nullptr || value->kind != json::Value::Kind::Array)
        return result;
    for (const json::Value& element : value->array)
        if (element.kind == json::Value::Kind::String)
            result.push_back(element.string);
    return result;
}

// Split "simple-project.build" into its fixture and command halves. The
// command is the final dotted component, so a fixture name may itself contain
// dots. A name with no dot leaves both empty and relies on the JSON, or on
// there being exactly one fixture.
void split_case_name(const std::string& stem, std::string& fixture, std::string& command)
{
    const std::size_t dot = stem.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == stem.size()) {
        command = stem;
        return;
    }
    fixture = stem.substr(0, dot);
    command = stem.substr(dot + 1);
}

// Read an optional string field, leaving `out` untouched when absent.
bool optional_string(const json::Value& object, const char* key, std::string& out)
{
    const json::Value* field = object.find(key);
    if (field == nullptr)
        return true;
    if (field->kind != json::Value::Kind::String)
        return false;
    out = field->string;
    return true;
}

CaseResult failure(std::string name, std::string detail)
{
    CaseResult result;
    result.name   = std::move(name);
    result.passed = false;
    result.detail = std::move(detail);
    return result;
}

// Run one case file end to end.
CaseResult run_case(const kpl::Plugin&           plugin,
                    const std::filesystem::path& tests_dir,
                    const std::string&           file_name)
{
    const std::string stem = file_name.substr(0, file_name.size() - kExpectedSuffix.size());
    const std::filesystem::path expected_path = tests_dir / "expected" / file_name;

    // The file name carries the case by convention — "<fixture>.<command>" —
    // and the JSON may override either half. The convention keeps the common
    // case free of boilerplate (design doc §5.2 writes just
    // "build.steps.json"); the override is what lets one command have several
    // cases, e.g. a "generator: auto with Ninja present" and a "without"
    // variant that would otherwise collide on the same file name.
    std::string fixture;
    std::string command;
    split_case_name(stem, fixture, command);

    try {
        const json::Value expectation =
            json::parse(fs::read_text(expected_path), expected_path.string());
        if (!optional_string(expectation, "fixture", fixture) ||
            !optional_string(expectation, "command", command))
            return failure(stem, "'fixture' and 'command' must be strings");

        // With no fixture named anywhere, fall back to the plugin's single
        // fixture directory if it has exactly one — ambiguity is refused
        // rather than guessed at.
        if (fixture.empty()) {
            const std::vector<std::string> all = fs::glob(tests_dir / "fixtures", "*");
            if (all.size() != 1)
                return failure(stem,
                               "no fixture named; add a \"fixture\" field or name the file "
                               "'<fixture>." +
                                   command + std::string(kExpectedSuffix) + "'");
            fixture = all.front();
        }

        const std::filesystem::path fixture_path = tests_dir / "fixtures" / fixture;
        if (!fs::is_dir(fixture_path))
            return failure(stem, "no fixture directory at " + fixture_path.string());

        // --- inputs ---------------------------------------------------------
        std::map<std::string, kpl::Value> overrides;
        if (const json::Value* config = expectation.find("config")) {
            if (config->kind != json::Value::Kind::Object)
                return failure(stem, "'config' must be an object");
            for (const auto& [key, value] : config->object) {
                const std::optional<kpl::Value> converted = config_value(value);
                if (!converted)
                    return failure(stem, "config key '" + key + "' has an unsupported type");
                overrides.emplace(key, *converted);
            }
        }

        const auto [config, config_errors] = kpl::build_config(plugin, overrides);
        if (!config_errors.empty()) {
            std::string detail = "config does not satisfy the schema:";
            for (const std::string& error : config_errors)
                detail += "\n        " + error;
            return failure(stem, detail);
        }

        const std::vector<std::string> extra = string_array(expectation.find("extra"));

        // Declared, not sampled: see the note in core/plugin.hpp on why
        // `tools` and `env` cannot come from the host if the test is to be
        // reproducible.
        const std::vector<std::string>     tools = string_array(expectation.find("tools"));
        std::map<std::string, std::string> environment;
        if (const json::Value* declared = expectation.find("env")) {
            if (declared->kind != json::Value::Kind::Object)
                return failure(stem, "'env' must be an object");
            for (const auto& [name, value] : declared->object)
                if (value.kind == json::Value::Kind::String)
                    environment.emplace(name, value.string);
        }

        kpl::Project project = kpl::host_project(fixture_path.string());
        project.tool         = [tools](std::string_view name) {
            return std::find(tools.begin(), tools.end(), std::string(name)) != tools.end();
        };
        project.env = [environment](std::string_view name) -> std::optional<std::string> {
            const auto found = environment.find(std::string(name));
            return found == environment.end() ? std::nullopt
                                              : std::optional<std::string>(found->second);
        };

        // --- run and compare -------------------------------------------------
        const kpl::CommandSpec actual = kpl::evaluate(plugin, command, project, config, extra);
        const kpl::CommandSpec wanted = kpl::spec_from_json(expectation, expected_path.string());

        // Compared as canonical text rather than field by field: to_json
        // always writes every field in sorted key order, so equal specs
        // serialize identically — and when they differ, the two renderings are
        // exactly the diff a plugin author wants to read.
        const std::string actual_text = json::write(kpl::to_json(actual));
        const std::string wanted_text = json::write(kpl::to_json(wanted));
        if (actual_text == wanted_text) {
            CaseResult result;
            result.name   = stem;
            result.passed = true;
            return result;
        }
        return failure(stem, "expected:\n" + wanted_text + "      actual:\n" + actual_text);
    }
    catch (const diag::Error& error) {
        // report() already ends with a newline and carries the file/line.
        return failure(stem, error.report());
    }
}

} // namespace

const char* source_name(Source source)
{
    switch (source) {
        case Source::ProjectLocal:
            return "project";
        case Source::SearchPath:
            return "path";
        case Source::User:
            return "user";
        case Source::Bundled:
            return "bundled";
        case Source::Repository:
            return "repo";
        case Source::Embedded:
            return "embedded";
    }
    return "unknown";
}

namespace
{

// Add every plugin directory under `dir` to `found`, unless a plugin of the
// same name is already there. "Already there" is what implements §6.5: tiers
// are visited highest-precedence first, so the first tier to claim a name is
// the one that wins and the rest are shadowed.
void collect_from(const std::filesystem::path& dir, Source source, std::vector<Located>& found)
{
    if (dir.empty() || !fs::is_dir(dir))
        return;
    for (const std::string& name : fs::glob(dir, "*")) {
        Located located;
        located.name      = name;
        located.directory = dir / name;
        located.manifest  = located.directory / "plugin.kpl";
        located.source    = source;
        // A directory with no plugin.kpl is not a plugin. Skipping it silently
        // keeps `registry/`-style siblings and editor scratch directories from
        // being reported as broken.
        if (!fs::is_file(located.manifest))
            continue;
        const bool shadowed = std::any_of(
            found.begin(), found.end(), [&name](const Located& seen) { return seen.name == name; });
        if (!shadowed)
            found.push_back(std::move(located));
    }
}

} // namespace

std::vector<Located> discover(const DiscoveryOptions& options)
{
    std::vector<Located> found;

    // §6.5, highest precedence first.
    if (!options.project_root.empty())
        collect_from(options.project_root / ".kap" / "plugins", Source::ProjectLocal, found);

    if (options.include_search_path) {
        std::vector<std::filesystem::path> entries = options.search_path;
        if (entries.empty())
            entries = paths::split_path_list(paths::env_or_empty("KAP_PLUGIN_PATH"));
        for (const std::filesystem::path& entry : entries)
            collect_from(entry, Source::SearchPath, found);
    }

    if (options.include_user)
        collect_from(paths::user_plugin_dir(), Source::User, found);

    if (options.include_bundled)
        collect_from(paths::bundled_plugin_dir(), Source::Bundled, found);

    // Then the repository's own plugins, so anything *installed* shadows the
    // in-repo copy of the same plugin.
    if (options.include_repository && !options.project_root.empty())
        collect_from(options.project_root / "plugins", Source::Repository, found);

    // Compiled-in plugins last of all. An embedded plugin is a snapshot taken
    // when the binary was compiled, which makes it the weakest claim there is:
    // a distributor's patched copy should win, and so should the plugin a
    // developer has open in their checkout.
    //
    // $KAP_NO_EMBEDDED_PLUGINS turns them off. Worth having as an escape hatch
    // rather than only as a build option: on an embedded binary every directory
    // has a plugin, which is exactly what makes "why is kap using a plugin I
    // never installed?" hard to answer. Setting it reproduces the behaviour of
    // a build without embedding, so the two can be compared directly.
    if (options.include_embedded && paths::env_or_empty("KAP_NO_EMBEDDED_PLUGINS").empty())
        collect_from(bundled::ensure_materialized(), Source::Embedded, found);

    // collect_from preserves tier order, not name order; sort so every caller
    // (and every test) sees a deterministic list.
    std::sort(found.begin(), found.end(), [](const Located& left, const Located& right) {
        return left.name < right.name;
    });
    return found;
}

std::vector<Located> discover(const std::filesystem::path& root)
{
    DiscoveryOptions options;
    options.project_root = root;
    return discover(options);
}

std::vector<CaseResult> run_tests(const Located& located, const std::filesystem::path& cache)
{
    std::vector<CaseResult> results;

    kpl::Plugin plugin;
    try {
        // Through the AST cache (§5.14) rather than kpl::parse directly, so
        // the cache is exercised by the same path real commands will use.
        plugin = kapc::load(located.manifest, cache).plugin;
    }
    catch (const diag::Error& error) {
        results.push_back(failure("<parse>", error.report()));
        return results;
    }

    // A plugin that does not type-check cannot produce a meaningful spec, so
    // report that once instead of failing every case with the same cause.
    const std::vector<std::string> type_errors = kpl::type_check(plugin);
    if (!type_errors.empty()) {
        std::string detail = "plugin does not type-check:";
        for (const std::string& error : type_errors)
            detail += "\n        " + error;
        results.push_back(failure("<typecheck>", detail));
        return results;
    }

    const std::filesystem::path tests_dir = located.directory / "tests";
    for (const std::string& file_name : fs::glob(tests_dir / "expected", "*.steps.json"))
        results.push_back(run_case(plugin, tests_dir, file_name));

    return results;
}

} // namespace plugin
} // namespace kap
