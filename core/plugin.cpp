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

#include "core/diag.hpp"
#include "core/fs.hpp"
#include "core/json.hpp"
#include "core/kapc.hpp"
#include "core/kpl.hpp"

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

std::vector<Located> discover(const std::filesystem::path& root)
{
    std::vector<Located>        found;
    const std::filesystem::path plugins_dir = root / "plugins";
    for (const std::string& name : fs::glob(plugins_dir, "*")) {
        Located located;
        located.name      = name;
        located.directory = plugins_dir / name;
        located.manifest  = located.directory / "plugin.kpl";
        // A directory with no plugin.kpl is not a plugin. Skipping it silently
        // keeps `registry/`-style siblings and editor scratch directories from
        // being reported as broken.
        if (fs::is_file(located.manifest))
            found.push_back(std::move(located));
    }
    // fs::glob already sorts, so `found` is sorted by name.
    return found;
}

namespace
{

std::optional<int> manifest_integer(const kpl::Plugin& plugin, const char* key)
{
    if (!plugin.manifest)
        return std::nullopt;
    for (const kpl::Statement& statement : plugin.manifest->statements) {
        if (statement.kind != kpl::Statement::Kind::Assignment || statement.name != key)
            continue;
        if (statement.expressions.empty())
            return std::nullopt;
        const kpl::Expr& value = statement.expressions.front();
        if (value.kind == kpl::Expr::Kind::Integer)
            return static_cast<int>(value.token.integer);
    }
    return std::nullopt;
}

std::vector<std::string> manifest_string_list(const kpl::Plugin& plugin, const char* key)
{
    std::vector<std::string> result;
    if (!plugin.manifest)
        return result;
    for (const kpl::Statement& statement : plugin.manifest->statements) {
        if (statement.kind != kpl::Statement::Kind::Assignment || statement.name != key)
            continue;
        if (statement.expressions.empty())
            return result;
        const kpl::Expr& value = statement.expressions.front();
        if (value.kind != kpl::Expr::Kind::List)
            return result;
        for (const kpl::Expr& item : value.children) {
            if (item.kind == kpl::Expr::Kind::String)
                result.push_back(item.token.text);
            else if (item.kind == kpl::Expr::Kind::Name)
                result.push_back(item.token.text);
        }
    }
    return result;
}

bool is_rule_satisfied(const std::filesystem::path&  root,
                       const std::string&            rule_name,
                       const std::vector<kpl::Expr>& expressions,
                       std::vector<std::string>&     matched_files)
{
    if (rule_name == "file_exists") {
        if (expressions.size() != 1 || expressions.front().kind != kpl::Expr::Kind::String)
            return false;
        const std::filesystem::path path = root / expressions.front().token.text;
        if (fs::exists(path)) {
            matched_files.push_back(expressions.front().token.text);
            return true;
        }
        return false;
    }
    if (rule_name == "dir_exists") {
        if (expressions.size() != 1 || expressions.front().kind != kpl::Expr::Kind::String)
            return false;
        const std::filesystem::path path = root / expressions.front().token.text;
        if (fs::is_dir(path)) {
            matched_files.push_back(expressions.front().token.text);
            return true;
        }
        return false;
    }
    if (rule_name == "file_exists_any") {
        if (expressions.size() != 1 || expressions.front().kind != kpl::Expr::Kind::List)
            return false;
        for (const kpl::Expr& item : expressions.front().children) {
            if (item.kind == kpl::Expr::Kind::String) {
                const std::filesystem::path path = root / item.token.text;
                if (fs::exists(path)) {
                    matched_files.push_back(item.token.text);
                    return true;
                }
            }
        }
        return false;
    }
    if (rule_name == "file_contains") {
        if (expressions.size() != 1 || expressions.front().kind != kpl::Expr::Kind::Record)
            return false;
        std::string path_text;
        std::string pattern_text;
        for (std::size_t index = 0; index < expressions.front().names.size(); ++index) {
            if (expressions.front().names[index] == "path" &&
                expressions.front().children[index].kind == kpl::Expr::Kind::String)
                path_text = expressions.front().children[index].token.text;
            if (expressions.front().names[index] == "pattern" &&
                expressions.front().children[index].kind == kpl::Expr::Kind::String)
                pattern_text = expressions.front().children[index].token.text;
        }
        if (path_text.empty() || pattern_text.empty())
            return false;
        const std::filesystem::path path = root / path_text;
        if (!fs::is_file(path))
            return false;
        try {
            const std::string contents = fs::read_text(path);
            if (contents.find(pattern_text) != std::string::npos) {
                matched_files.push_back(path_text);
                return true;
            }
        }
        catch (const diag::Error&) {
            return false;
        }
        return false;
    }
    return false;
}

std::vector<std::string> cached_match_names(const std::filesystem::path& cache_path,
                                            const std::filesystem::path& root)
{
    if (!fs::exists(cache_path))
        return {};
    try {
        const json::Value doc = json::parse(fs::read_text(cache_path), cache_path.string());
        if (doc.kind != json::Value::Kind::Object)
            return {};
        const json::Value* root_value  = doc.find("root");
        const json::Value* names_value = doc.find("plugins");
        if (root_value == nullptr || root_value->kind != json::Value::Kind::String ||
            names_value == nullptr || names_value->kind != json::Value::Kind::Array)
            return {};
        if (root_value->string != root.string())
            return {};

        std::vector<std::string> names;
        for (const json::Value& entry : names_value->array) {
            if (entry.kind == json::Value::Kind::String)
                names.push_back(entry.string);
        }
        return names;
    }
    catch (const diag::Error&) {
        return {};
    }
}

void write_detection_cache(const std::filesystem::path&       root,
                           const std::vector<DetectionMatch>& matches)
{
    const std::filesystem::path cache_dir  = root / ".kap";
    const std::filesystem::path cache_path = cache_dir / "cache.json";
    std::filesystem::create_directories(cache_dir);
    std::vector<json::Value> plugins;
    plugins.reserve(matches.size());
    for (const DetectionMatch& match : matches) {
        plugins.push_back(json::make_string(match.located.name));
    }
    const json::Value doc = json::make_object({
        {"root", json::make_string(root.string())},
        {"plugins", json::make_array(std::move(plugins))},
    });
    std::ofstream     out(cache_path, std::ios::binary | std::ios::trunc);
    out << json::write(doc, true);
}

} // namespace

std::vector<DetectionMatch> detect(const std::filesystem::path& root)
{
    const std::filesystem::path cache_path = root / ".kap" / "cache.json";
    if (const auto cached = cached_match_names(cache_path, root); !cached.empty()) {
        std::vector<DetectionMatch> result;
        const std::vector<Located>  discovered = discover(root);
        for (const Located& located : discovered) {
            const auto it = std::find(cached.begin(), cached.end(), located.name);
            if (it != cached.end()) {
                DetectionMatch match;
                match.located = located;
                match.score = manifest_integer(kapc::load(located.manifest, {}).plugin, "priority")
                                  .value_or(0);
                result.push_back(match);
            }
        }
        return result;
    }

    std::vector<DetectionMatch> matches;
    for (const Located& located : discover(root)) {
        try {
            const kpl::Plugin        plugin   = kapc::load(located.manifest, {}).plugin;
            const std::optional<int> priority = manifest_integer(plugin, "priority");
            if (!priority)
                continue;

            std::vector<std::string> matched_files;
            bool                     matched = false;
            if (plugin.detect) {
                for (const kpl::Statement& statement : plugin.detect->statements) {
                    if (statement.kind != kpl::Statement::Kind::Directive)
                        continue;
                    if (is_rule_satisfied(
                            root, statement.name, statement.expressions, matched_files)) {
                        matched = true;
                    }
                }
            }
            if (matched) {
                DetectionMatch match;
                match.located       = located;
                match.score         = *priority;
                match.matched_files = matched_files;
                matches.push_back(match);
            }
        }
        catch (const diag::Error&) {
            continue;
        }
    }

    std::vector<DetectionMatch> survivors;
    for (DetectionMatch& candidate : matches) {
        bool superseded = false;
        for (const DetectionMatch& other : matches) {
            if (&candidate == &other)
                continue;
            const std::vector<std::string> others =
                manifest_string_list(kapc::load(other.located.manifest, {}).plugin, "supersedes");
            if (std::find(others.begin(), others.end(), candidate.located.name) != others.end()) {
                superseded = true;
                break;
            }
        }
        if (!superseded)
            survivors.push_back(candidate);
    }

    if (survivors.empty()) {
        write_detection_cache(root, {});
        return {};
    }

    std::sort(survivors.begin(),
              survivors.end(),
              [](const DetectionMatch& left, const DetectionMatch& right) {
                  if (left.score != right.score)
                      return left.score > right.score;
                  return left.located.name < right.located.name;
              });
    if (survivors.size() > 1 && survivors.front().score == survivors[1].score) {
        throw diag::Error{diag::error("detection tie: multiple plugins match at priority " +
                                      std::to_string(survivors.front().score) +
                                      "; pin one in kap.toml")};
    }

    write_detection_cache(root, survivors);
    return {survivors.front()};
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
