// core/detect.cpp
//
// Implementation of the detection engine declared in core/detect.hpp
// (design doc §3, Milestone 4).

#include "core/detect.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#include "core/fs.hpp"
#include "core/hash.hpp"
#include "core/json.hpp"
#include "core/kapc.hpp"
#include "core/paths.hpp"

namespace kap
{
namespace detect
{

namespace
{

// Bump when the cache document's shape changes. An entry written by a
// different kap is then simply ignored, which is the right answer for a cache:
// a stale hit is a wrong answer, a miss is only a slower one.
constexpr int kCacheVersion = 2;

// --- manifest readers -----------------------------------------------------------
//
// The manifest block is a list of `key = value` assignments (§5.3). The parser
// keeps it generic so KPL can grow new manifest keys without touching the
// grammar, which means the engine has to pick the fields it cares about back
// out of the statement list. These three helpers are that projection.

const kpl::Expr* manifest_value(const kpl::Plugin& plugin, const char* key)
{
    if (!plugin.manifest)
        return nullptr;
    for (const kpl::Statement& statement : plugin.manifest->statements) {
        if (statement.kind != kpl::Statement::Kind::Assignment || statement.name != key)
            continue;
        if (statement.expressions.empty())
            return nullptr;
        return &statement.expressions.front();
    }
    return nullptr;
}

std::optional<int> manifest_integer(const kpl::Plugin& plugin, const char* key)
{
    const kpl::Expr* value = manifest_value(plugin, key);
    if (value == nullptr || value->kind != kpl::Expr::Kind::Integer)
        return std::nullopt;
    return static_cast<int>(value->token.integer);
}

std::string manifest_string(const kpl::Plugin& plugin, const char* key)
{
    const kpl::Expr* value = manifest_value(plugin, key);
    if (value == nullptr || value->kind != kpl::Expr::Kind::String)
        return {};
    return value->token.text;
}

bool manifest_boolean(const kpl::Plugin& plugin, const char* key, bool fallback)
{
    const kpl::Expr* value = manifest_value(plugin, key);
    if (value == nullptr || value->kind != kpl::Expr::Kind::Boolean)
        return fallback;
    // The lexer keeps `true`/`false` as identifier text on the token, so the
    // boolean's value is read from there rather than from a dedicated field.
    return value->token.text == "true";
}

std::vector<std::string> manifest_string_list(const kpl::Plugin& plugin, const char* key)
{
    std::vector<std::string> result;
    const kpl::Expr*         value = manifest_value(plugin, key);
    if (value == nullptr || value->kind != kpl::Expr::Kind::List)
        return result;
    for (const kpl::Expr& item : value->children) {
        // A list element may be quoted or a bare word; §5.5 reads a bare
        // identifier inside a directive list as a string, and `supersedes` is
        // written both ways in the wild.
        if (item.kind == kpl::Expr::Kind::String || item.kind == kpl::Expr::Kind::Name)
            result.push_back(item.token.text);
    }
    return result;
}

diag::Location location_of(const kpl::Plugin& plugin, const kpl::Statement& statement)
{
    return diag::Location{plugin.source_name, statement.token.line, statement.token.column};
}

// --- filesystem helpers ---------------------------------------------------------

// A file's modification time as an integer count of nanoseconds. Integral
// rather than a formatted timestamp because core/json.hpp deliberately has no
// floating-point support (§9) and because an integer compares exactly.
std::int64_t mtime_of(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto      stamp = std::filesystem::last_write_time(path, ec);
    if (ec)
        return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count();
}

// Resolve a plugin-declared marker path against the project root, refusing
// anything that would escape it.
//
// A detect rule is untrusted input in exactly the same way a `project.read`
// argument is (§7), and it runs *before* any sandbox the interpreter sets up —
// so `file_exists "../../../../etc/passwd"` has to be refused here or nowhere.
// Returning nullopt makes the rule simply not match, which is the correct
// outcome: a plugin cannot learn anything about the world outside the project
// by probing it.
std::optional<std::filesystem::path> resolve_marker(const std::filesystem::path& root,
                                                    const std::string&           relative)
{
    if (relative.empty())
        return std::nullopt;
    const std::filesystem::path candidate(relative);
    if (candidate.is_absolute())
        return std::nullopt;
    const std::filesystem::path joined = root / candidate;
    if (!fs::is_within(root, joined))
        return std::nullopt;
    return joined;
}

// A marker path may itself contain a wildcard — §3.2's `file_exists_any`
// example is ["*.xcworkspace", "*.xcodeproj"]. Expand only the final
// component, matching the same restriction core/fs.hpp's glob applies.
std::vector<std::string> expand_marker(const std::filesystem::path& root,
                                       const std::string&           relative)
{
    if (relative.find('*') == std::string::npos && relative.find('?') == std::string::npos) {
        return {relative};
    }
    const std::filesystem::path pattern(relative);
    const std::string           leaf   = pattern.filename().string();
    const std::filesystem::path parent = pattern.parent_path();
    if (leaf.find('*') == std::string::npos && leaf.find('?') == std::string::npos) {
        // The wildcard is in a directory component, which fs::glob cannot
        // expand. Rather than half-supporting it, treat the rule as matching
        // nothing; `compile()` has already rejected the obviously-broken cases.
        return {};
    }
    const std::optional<std::filesystem::path> dir =
        resolve_marker(root, parent.empty() ? "." : parent.string());
    if (!dir)
        return {};

    std::vector<std::string> results;
    for (const std::string& name : fs::glob(*dir, leaf))
        results.push_back(parent.empty() ? name : (parent / name).string());
    return results;
}

} // namespace

RuleTable compile(const kpl::Plugin& plugin, const std::string& fallback_name)
{
    RuleTable table;
    table.name        = manifest_string(plugin, "name");
    table.version     = manifest_string(plugin, "version");
    table.api_version = manifest_integer(plugin, "api_version").value_or(1);
    table.priority    = manifest_integer(plugin, "priority").value_or(0);
    table.composable  = manifest_boolean(plugin, "composable", false);
    table.supersedes  = manifest_string_list(plugin, "supersedes");
    if (table.name.empty())
        table.name = fallback_name;

    if (!plugin.detect)
        return table;

    for (const kpl::Statement& statement : plugin.detect->statements) {
        if (statement.kind != kpl::Statement::Kind::Directive)
            continue;

        Rule rule;
        rule.location = location_of(plugin, statement);

        const auto require_one_string = [&](Rule::Kind kind) {
            if (statement.expressions.size() != 1 ||
                statement.expressions.front().kind != kpl::Expr::Kind::String) {
                throw diag::Error{diag::error(
                    "detect rule '" + statement.name + "' takes exactly one string path",
                    rule.location,
                    {"example: " + statement.name + " \"CMakeLists.txt\""})};
            }
            rule.kind = kind;
            rule.patterns.push_back(statement.expressions.front().token.text);
        };

        if (statement.name == "file_exists") {
            require_one_string(Rule::Kind::FileExists);
        } else if (statement.name == "dir_exists") {
            require_one_string(Rule::Kind::DirExists);
        } else if (statement.name == "file_exists_any") {
            if (statement.expressions.size() != 1 ||
                statement.expressions.front().kind != kpl::Expr::Kind::List) {
                throw diag::Error{
                    diag::error("detect rule 'file_exists_any' takes exactly one list of paths",
                                rule.location,
                                {R"(example: file_exists_any ["*.xcworkspace", "*.xcodeproj"])"})};
            }
            rule.kind = Rule::Kind::FileExistsAny;
            for (const kpl::Expr& item : statement.expressions.front().children) {
                if (item.kind != kpl::Expr::Kind::String && item.kind != kpl::Expr::Kind::Name) {
                    throw diag::Error{
                        diag::error("detect rule 'file_exists_any' takes a list of path strings",
                                    rule.location)};
                }
                rule.patterns.push_back(item.token.text);
            }
            if (rule.patterns.empty()) {
                throw diag::Error{
                    diag::error("detect rule 'file_exists_any' needs at least one path",
                                rule.location,
                                {"an empty list can never match, which is almost certainly "
                                 "not what the plugin means"})};
            }
        } else if (statement.name == "file_contains") {
            if (statement.expressions.size() != 1 ||
                statement.expressions.front().kind != kpl::Expr::Kind::Record) {
                throw diag::Error{diag::error(
                    "detect rule 'file_contains' takes exactly one record",
                    rule.location,
                    {R"(example: file_contains { path: "package.json", pattern: "\"workspaces\"" })"})};
            }
            const kpl::Expr& record = statement.expressions.front();
            std::string      path_text;
            std::string      needle;
            for (std::size_t index = 0; index < record.names.size(); ++index) {
                const kpl::Expr& field = record.children[index];
                if (field.kind != kpl::Expr::Kind::String && field.kind != kpl::Expr::Kind::Name)
                    continue;
                if (record.names[index] == "path")
                    path_text = field.token.text;
                else if (record.names[index] == "pattern")
                    needle = field.token.text;
            }
            if (path_text.empty() || needle.empty()) {
                throw diag::Error{diag::error(
                    "detect rule 'file_contains' needs both a 'path' and a 'pattern' string",
                    rule.location)};
            }
            rule.kind = Rule::Kind::FileContains;
            rule.patterns.push_back(path_text);
            rule.pattern = needle;
        } else {
            // Silently ignoring an unknown rule would produce a plugin that
            // never matches with nothing to explain why, which is the worst
            // possible failure mode for a plugin author.
            throw diag::Error{diag::error(
                "unknown detect rule '" + statement.name + "'",
                rule.location,
                {"expected one of: file_exists, file_exists_any, dir_exists, file_contains"})};
        }

        table.rules.push_back(std::move(rule));
    }
    return table;
}

bool evaluate(const Rule&                  rule,
              const std::filesystem::path& root,
              std::vector<std::string>&    matched_files)
{
    switch (rule.kind) {
        case Rule::Kind::FileExists:
        case Rule::Kind::FileExistsAny:
            {
                // The two share an implementation: `file_exists` is the
                // single-pattern case of `file_exists_any`, and collapsing them
                // means the wildcard handling cannot drift between the two.
                for (const std::string& pattern : rule.patterns) {
                    for (const std::string& candidate : expand_marker(root, pattern)) {
                        const auto resolved = resolve_marker(root, candidate);
                        if (resolved && fs::exists(*resolved)) {
                            matched_files.push_back(candidate);
                            return true;
                        }
                    }
                }
                return false;
            }
        case Rule::Kind::DirExists:
            {
                for (const std::string& pattern : rule.patterns) {
                    for (const std::string& candidate : expand_marker(root, pattern)) {
                        const auto resolved = resolve_marker(root, candidate);
                        if (resolved && fs::is_dir(*resolved)) {
                            matched_files.push_back(candidate);
                            return true;
                        }
                    }
                }
                return false;
            }
        case Rule::Kind::FileContains:
            {
                const auto resolved = resolve_marker(root, rule.patterns.front());
                if (!resolved || !fs::is_file(*resolved))
                    return false;
                try {
                    // Inherits fs::read_text's 1 MiB cap (§7), so a detect rule
                    // pointed at a huge file fails fast instead of hanging.
                    const std::string contents = fs::read_text(*resolved);
                    if (contents.find(rule.pattern) == std::string::npos)
                        return false;
                    matched_files.push_back(rule.patterns.front());
                    return true;
                }
                catch (const diag::Error&) {
                    return false;
                }
            }
    }
    return false;
}

std::filesystem::path cache_file(const std::filesystem::path& root)
{
    return root / ".kap" / "cache.json";
}

void invalidate_cache(const std::filesystem::path& root)
{
    std::error_code ec;
    std::filesystem::remove(cache_file(root), ec); // missing file is success
}

namespace
{

// --- the cache (§3.2 step 5) -----------------------------------------------------
//
// Two independent guards, because they catch different mistakes:
//
//   * The *precheck key* hashes what is knowable before any scan — the names
//     of the entries directly under the root (which change when a marker file
//     is created, deleted, or renamed) and every candidate plugin's identity
//     and manifest mtime (which change when a plugin is installed, edited,
//     removed, or disabled).
//
//   * The *marker list* records the mtime of every file that actually fired,
//     and is re-checked on load. That is the guard §3.2 names literally, and
//     it is what catches a `file_contains` rule whose file was edited in place.
//
// Either one alone leaves a hole: markers alone miss a newly added Cargo.toml
// (no existing marker changed), and the precheck alone misses a file *edited*
// in place, which is what a `file_contains` rule reads. Together they cover
// everything except a marker created deep inside a subdirectory that no
// existing rule already watches; `kap detect --refresh` covers that.

std::string precheck_key(const std::filesystem::path&        root,
                         const std::vector<plugin::Located>& plugins)
{
    std::string material = root.string();

    // The names of the entries directly under the root, sorted — not the
    // root's own mtime.
    //
    // The mtime is the obvious choice and it is wrong here, for a reason worth
    // spelling out: writing the cache *creates* `.kap/`, which bumps the
    // root's mtime, so the key stored in a fresh entry never matches the key
    // computed on the very next run. The cache would invalidate itself on
    // every write and never once hit.
    //
    // A listing has neither problem, and is strictly more precise: it changes
    // when a file is created, deleted, or renamed at the top level — exactly
    // the events that can turn a non-matching rule into a matching one — and
    // is immune to filesystem mtime granularity. `.kap` is excluded because it
    // is kap's own derived state; including it would reintroduce the loop.
    material += "|entries:";
    for (const std::string& entry : fs::glob(root, "*")) {
        if (entry == ".kap")
            continue;
        material += entry;
        material += ",";
    }

    // Plugin identity and manifest mtime: installing, removing, or editing a
    // plugin changes what the answer should be even when the project has not
    // changed at all.
    for (const plugin::Located& located : plugins) {
        material += "|p:" + located.name + ":" + located.manifest.string() + ":" +
                    std::to_string(mtime_of(located.manifest)) + ":" +
                    (located.enabled ? "on" : "off");
    }
    return hash::hex64(hash::fnv1a64(material));
}

// Read a cache entry, returning nothing unless every guard above agrees.
std::optional<std::vector<Match>> read_cache(const std::filesystem::path&        root,
                                             const std::vector<plugin::Located>& plugins,
                                             const std::string&                  key)
{
    const std::filesystem::path path = cache_file(root);
    if (!fs::is_file(path))
        return std::nullopt;

    json::Value doc;
    try {
        doc = json::parse(fs::read_text(path), path.string());
    }
    catch (const diag::Error&) {
        return std::nullopt; // a corrupt cache is a miss, never a failure
    }
    if (doc.kind != json::Value::Kind::Object)
        return std::nullopt;

    const json::Value* version     = doc.find("version");
    const json::Value* stored_root = doc.find("root");
    const json::Value* stored_key  = doc.find("key");
    const json::Value* entries     = doc.find("matches");
    if (version == nullptr || version->kind != json::Value::Kind::Integer ||
        version->integer != kCacheVersion)
        return std::nullopt;
    if (stored_root == nullptr || stored_root->kind != json::Value::Kind::String ||
        stored_root->string != root.string())
        return std::nullopt;
    if (stored_key == nullptr || stored_key->kind != json::Value::Kind::String ||
        stored_key->string != key)
        return std::nullopt;
    if (entries == nullptr || entries->kind != json::Value::Kind::Array)
        return std::nullopt;

    // An entry names plugins by name; they must still resolve to the same file
    // on disk, or the cached answer describes a plugin that is no longer there.
    const auto find_plugin = [&plugins](const std::string& name) -> const plugin::Located* {
        for (const plugin::Located& located : plugins)
            if (located.name == name)
                return &located;
        return nullptr;
    };

    std::vector<Match> matches;
    for (const json::Value& entry : entries->array) {
        if (entry.kind != json::Value::Kind::Object)
            return std::nullopt;
        const json::Value* name       = entry.find("name");
        const json::Value* priority   = entry.find("priority");
        const json::Value* score      = entry.find("score");
        const json::Value* composable = entry.find("composable");
        const json::Value* markers    = entry.find("markers");
        if (name == nullptr || name->kind != json::Value::Kind::String || priority == nullptr ||
            priority->kind != json::Value::Kind::Integer || score == nullptr ||
            score->kind != json::Value::Kind::Integer || composable == nullptr ||
            composable->kind != json::Value::Kind::Boolean || markers == nullptr ||
            markers->kind != json::Value::Kind::Array)
            return std::nullopt;

        const plugin::Located* located = find_plugin(name->string);
        if (located == nullptr)
            return std::nullopt;

        Match match;
        match.located    = *located;
        match.name       = name->string;
        match.priority   = static_cast<int>(priority->integer);
        match.score      = static_cast<int>(score->integer);
        match.composable = composable->boolean;

        for (const json::Value& marker : markers->array) {
            if (marker.kind != json::Value::Kind::Object)
                return std::nullopt;
            const json::Value* marker_path = marker.find("path");
            const json::Value* marker_time = marker.find("mtime");
            if (marker_path == nullptr || marker_path->kind != json::Value::Kind::String ||
                marker_time == nullptr || marker_time->kind != json::Value::Kind::Integer)
                return std::nullopt;

            const auto resolved = resolve_marker(root, marker_path->string);
            if (!resolved || !fs::exists(*resolved))
                return std::nullopt; // the marker is gone: rescan
            if (mtime_of(*resolved) != marker_time->integer)
                return std::nullopt; // the marker changed: rescan
            match.matched_files.push_back(marker_path->string);
        }
        matches.push_back(std::move(match));
    }
    return matches;
}

void write_cache(const std::filesystem::path& root,
                 const std::string&           key,
                 const std::vector<Match>&    matches)
{
    // Writing the cache must never be able to fail a command. A read-only
    // checkout, a full disk, or a project on a filesystem that does not permit
    // the directory are all "no cache today", not "kap build failed".
    try {
        const std::filesystem::path dir = root / ".kap";
        std::error_code             ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return;

        // Make the cache directory self-ignoring. §3.2 calls .kap/cache.json
        // gitignored, but kap cannot edit the user's .gitignore without
        // touching a file they own; a .gitignore *inside* the directory is the
        // standard way to keep generated state out of a commit without that.
        const std::filesystem::path ignore = dir / ".gitignore";
        if (!fs::exists(ignore)) {
            std::ofstream out(ignore, std::ios::binary | std::ios::trunc);
            out << "# Created by kap. Everything in this directory is derived state.\n"
                   "# Project-local plugins (.kap/plugins) are the one exception.\n"
                   "*\n"
                   "!.gitignore\n"
                   "!plugins/\n";
        }

        std::vector<json::Value> entries;
        entries.reserve(matches.size());
        for (const Match& match : matches) {
            std::vector<json::Value> markers;
            markers.reserve(match.matched_files.size());
            for (const std::string& marker : match.matched_files) {
                const auto resolved = resolve_marker(root, marker);
                markers.push_back(json::make_object({
                    {"mtime", json::make_integer(resolved ? mtime_of(*resolved) : 0)},
                    {"path", json::make_string(marker)},
                }));
            }
            entries.push_back(json::make_object({
                {"composable", json::make_boolean(match.composable)},
                {"markers", json::make_array(std::move(markers))},
                {"name", json::make_string(match.name)},
                {"priority", json::make_integer(match.priority)},
                {"score", json::make_integer(match.score)},
            }));
        }

        const json::Value doc = json::make_object({
            {"key", json::make_string(key)},
            {"matches", json::make_array(std::move(entries))},
            {"root", json::make_string(root.string())},
            {"version", json::make_integer(kCacheVersion)},
        });

        // Write-then-rename, so a reader never sees a half-written document.
        const std::filesystem::path final_path = cache_file(root);
        const std::filesystem::path temp_path  = final_path.string() + ".tmp";
        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            if (!out)
                return;
            out << json::write(doc, true);
            if (!out)
                return;
        }
        std::filesystem::rename(temp_path, final_path, ec);
        if (ec)
            std::filesystem::remove(temp_path, ec);
    }
    catch (const std::exception&) {
        // Same reasoning: a cache is an optimisation and may never be the
        // reason a command fails.
    }
}

// Evaluate every plugin's rules against one directory. Plugins that cannot be
// loaded, or that are too new (§12 Q2), are skipped with a warning rather than
// aborting: an unrelated broken plugin must not break `kap build` here.
std::vector<Match> scan_directory(const std::filesystem::path&        root,
                                  const std::vector<plugin::Located>& plugins,
                                  const std::filesystem::path&        ast_cache,
                                  std::vector<std::string>&           warnings)
{
    std::vector<Match> matches;
    for (const plugin::Located& located : plugins) {
        if (!located.enabled)
            continue;
        try {
            const kpl::Plugin parsed = kapc::load(located.manifest, ast_cache).plugin;
            const RuleTable   table  = compile(parsed, located.name);

            if (table.api_version > kSupportedApiVersion) {
                warnings.push_back("skipping plugin '" + table.name +
                                   "': it declares api_version " +
                                   std::to_string(table.api_version) + " but this kap supports " +
                                   std::to_string(kSupportedApiVersion));
                continue;
            }
            if (table.rules.empty())
                continue; // a plugin with no detect block claims no project

            Match match;
            match.located    = located;
            match.name       = table.name;
            match.priority   = table.priority;
            match.composable = table.composable;
            for (const Rule& rule : table.rules) {
                if (evaluate(rule, root, match.matched_files))
                    ++match.score;
            }
            if (match.score > 0)
                matches.push_back(std::move(match));
        }
        catch (const diag::Error& error) {
            warnings.push_back("skipping plugin '" + located.name +
                               "': " + error.diagnostic().message);
        }
    }
    return matches;
}

// §3.2 step 3/4: drop anything another matched plugin supersedes.
std::vector<Match> remove_superseded(const std::vector<Match>&    matches,
                                     const std::filesystem::path& ast_cache)
{
    // Collect the union of every matched plugin's `supersedes` list once.
    // Doing it per-candidate (the obvious nested loop) re-parses every plugin
    // O(n^2) times for an answer that does not depend on the candidate.
    std::vector<std::string> superseded;
    for (const Match& match : matches) {
        try {
            const kpl::Plugin parsed = kapc::load(match.located.manifest, ast_cache).plugin;
            for (std::string& name : manifest_string_list(parsed, "supersedes"))
                superseded.push_back(std::move(name));
        }
        catch (const diag::Error&) {
            // Already matched, so it parsed a moment ago; if it somehow cannot
            // be re-read, the conservative answer is "supersedes nothing".
        }
    }
    std::vector<Match> survivors;
    for (const Match& match : matches) {
        if (std::find(superseded.begin(), superseded.end(), match.name) == superseded.end())
            survivors.push_back(match);
    }
    return survivors;
}

} // namespace

Resolution resolve(const std::filesystem::path&        start,
                   const std::vector<plugin::Located>& plugins,
                   const Options&                      options)
{
    Resolution resolution;

    std::error_code             ec;
    const std::filesystem::path absolute_start = std::filesystem::weakly_canonical(start, ec);
    std::filesystem::path       root           = ec ? start : absolute_start;
    resolution.root                            = root;

    // §3.2 step 1: walk upward at most max_walk_up levels, stopping at the
    // first directory where anything matches. Level 0 is the starting
    // directory itself, so a max_walk_up of 0 means "here and nowhere else".
    const int levels = options.max_walk_up < 0 ? 0 : options.max_walk_up;
    for (int level = 0; level <= levels; ++level) {
        resolution.root = root;

        const std::string key = precheck_key(root, plugins);
        if (options.read_cache) {
            if (auto cached = read_cache(root, plugins, key); cached) {
                resolution.matches    = std::move(*cached);
                resolution.from_cache = true;
                return resolution;
            }
        }

        std::vector<Match> matches =
            scan_directory(root, plugins, options.ast_cache, resolution.warnings);

        if (!matches.empty()) {
            matches = remove_superseded(matches, options.ast_cache);
        }

        if (matches.empty()) {
            // Nothing here. Walking up is only worth it when a level remains;
            // at the last level this is the final answer and worth caching, so
            // a repeated `kap build` in a non-project directory stays cheap.
            if (level == levels) {
                if (options.write_cache)
                    write_cache(root, key, {});
                return resolution;
            }
            const std::filesystem::path parent = root.parent_path();
            if (parent.empty() || parent == root)
                return resolution; // reached the filesystem root
            root = parent;
            continue;
        }

        // §3.3: composable plugins ride alongside the winner rather than
        // competing with it, so they are set aside before the tie check.
        std::vector<Match> exclusive;
        std::vector<Match> sidecars;
        for (Match& match : matches) {
            if (match.composable)
                sidecars.push_back(std::move(match));
            else
                exclusive.push_back(std::move(match));
        }

        const auto by_priority_then_name = [](const Match& left, const Match& right) {
            if (left.priority != right.priority)
                return left.priority > right.priority;
            return left.name < right.name;
        };
        std::sort(exclusive.begin(), exclusive.end(), by_priority_then_name);
        std::sort(sidecars.begin(), sidecars.end(), by_priority_then_name);

        std::vector<Match> chosen;
        if (!exclusive.empty()) {
            // §3.2 step 4: an explicit pin wins outright and never ties.
            const auto pinned =
                options.pinned.empty()
                    ? exclusive.end()
                    : std::find_if(exclusive.begin(), exclusive.end(), [&options](const Match& m) {
                          return m.name == options.pinned;
                      });

            if (pinned != exclusive.end()) {
                chosen.push_back(*pinned);
            } else {
                if (!options.pinned.empty()) {
                    // A pin that names a plugin which did not match is a
                    // configuration error worth surfacing, not silence.
                    resolution.warnings.push_back(
                        "kap.toml pins detect.ecosystem = \"" + options.pinned +
                        "\", but that plugin did not match this directory");
                }
                if (exclusive.size() > 1 && exclusive[0].priority == exclusive[1].priority) {
                    // §3.2 step 4: never silently guess on a real tie.
                    std::string names;
                    for (const Match& match : exclusive) {
                        if (match.priority != exclusive[0].priority)
                            break;
                        if (!names.empty())
                            names += ", ";
                        names += match.name;
                    }
                    throw diag::Error{
                        diag::error("cannot tell which plugin owns " + root.string(),
                                    diag::Location{root.string(), -1, -1},
                                    {"these plugins matched at the same priority (" +
                                         std::to_string(exclusive[0].priority) + "): " + names,
                                     "pin one in kap.toml:",
                                     "    [detect]",
                                     "    ecosystem = \"" + exclusive[0].name + "\""})};
                }
                chosen.push_back(exclusive.front());
            }
        }
        for (Match& sidecar : sidecars)
            chosen.push_back(std::move(sidecar));

        resolution.matches = std::move(chosen);
        if (options.write_cache)
            write_cache(root, key, resolution.matches);
        return resolution;
    }

    return resolution;
}

} // namespace detect
} // namespace kap
