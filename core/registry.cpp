// core/registry.cpp
//
// Implementation of the plugin manager declared in core/registry.hpp
// (design doc §6, Milestone 7).

#include "core/registry.hpp"

#include <algorithm>
#include <fstream>
#include <utility>

#include <unistd.h>

#include "core/bundled.hpp"
#include "core/detect.hpp"
#include "core/diag.hpp"
#include "core/exec.hpp"
#include "core/fs.hpp"
#include "core/kpl.hpp"
#include "core/paths.hpp"
#include "core/sha256.hpp"
#include "core/version.hpp"

namespace kap
{
namespace registry
{

namespace
{

// --- small TOML readers ------------------------------------------------------------

const toml::Value* field(const toml::Value& table, const std::string& key)
{
    if (table.kind != toml::Value::Kind::Table)
        return nullptr;
    const auto found = table.table.find(key);
    return found == table.table.end() ? nullptr : &found->second;
}

std::string
string_field(const toml::Value& table, const std::string& key, const std::string& fallback = {})
{
    const toml::Value* value = field(table, key);
    return (value != nullptr && value->kind == toml::Value::Kind::String) ? value->str : fallback;
}

bool boolean_field(const toml::Value& table, const std::string& key, bool fallback)
{
    const toml::Value* value = field(table, key);
    return (value != nullptr && value->kind == toml::Value::Kind::Boolean) ? value->boolean
                                                                           : fallback;
}

std::vector<std::string> string_list_field(const toml::Value& table, const std::string& key)
{
    std::vector<std::string> items;
    const toml::Value*       value = field(table, key);
    if (value == nullptr || value->kind != toml::Value::Kind::Array)
        return items;
    for (const toml::Value& element : value->array)
        if (element.kind == toml::Value::Kind::String)
            items.push_back(element.str);
    return items;
}

// --- payload hashing ------------------------------------------------------------------

// Hash every file in a plugin directory, in sorted path order, including the
// paths themselves.
//
// Including the *names* matters: hashing only the contents would give the same
// digest to a plugin whose `plugin.kpl` and `README.md` had been swapped, and
// the file kap executes is chosen by name. Sorted order is what makes the
// digest reproducible on a different filesystem.
//
// `.git` is skipped: a shallow clone's object store differs between two clones
// of the same commit, so including it would make every checksum unrepeatable.
std::optional<std::string> hash_directory(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path>            files;
    std::error_code                               ec;
    std::filesystem::recursive_directory_iterator it(
        directory, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec)
        return std::nullopt;

    for (; it != end; it.increment(ec)) {
        if (ec)
            return std::nullopt;
        const std::filesystem::path relative = std::filesystem::relative(it->path(), directory, ec);
        if (ec)
            return std::nullopt;
        if (!relative.empty() && relative.begin()->string() == ".git") {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && !entry_ec)
            files.push_back(relative);
    }

    std::sort(files.begin(), files.end());

    sha256::Hasher hasher;
    for (const std::filesystem::path& relative : files) {
        hasher.update(relative.generic_string());
        hasher.update(std::string_view("\0", 1));
        try {
            hasher.update(fs::read_text(directory / relative));
        }
        catch (const diag::Error&) {
            return std::nullopt;
        }
        hasher.update(std::string_view("\0", 1));
    }

    const sha256::Digest  digest    = hasher.finish();
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out       = "sha256:";
    for (const std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

// --- copying ---------------------------------------------------------------------------

// Copy a plugin directory, skipping `.git`. Returns false on any error.
bool copy_plugin(const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec)
        return false;

    std::filesystem::recursive_directory_iterator it(
        from, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec)
        return false;

    for (; it != end; it.increment(ec)) {
        if (ec)
            return false;
        const std::filesystem::path relative = std::filesystem::relative(it->path(), from, ec);
        if (ec)
            return false;
        if (!relative.empty() && relative.begin()->string() == ".git") {
            if (it->is_directory())
                it.disable_recursion_pending();
            continue;
        }
        const std::filesystem::path target = to / relative;
        std::error_code             entry_ec;
        if (it->is_directory(entry_ec) && !entry_ec) {
            std::filesystem::create_directories(target, entry_ec);
        } else {
            std::filesystem::create_directories(target.parent_path(), entry_ec);
            std::filesystem::copy_file(
                it->path(), target, std::filesystem::copy_options::overwrite_existing, entry_ec);
        }
        if (entry_ec)
            return false;
    }
    return true;
}

// The name a plugin declares in its manifest, which is what it should be
// installed as — a git URL's last path component is only a guess.
std::string manifest_name(const std::filesystem::path& directory)
{
    try {
        const kpl::Plugin parsed = kpl::parse(fs::read_text(directory / "plugin.kpl"),
                                              (directory / "plugin.kpl").string());
        if (parsed.manifest) {
            for (const kpl::Statement& statement : parsed.manifest->statements) {
                if (statement.kind == kpl::Statement::Kind::Assignment &&
                    statement.name == "name" && !statement.expressions.empty() &&
                    statement.expressions.front().kind == kpl::Expr::Kind::String)
                    return statement.expressions.front().token.text;
            }
        }
    }
    catch (const diag::Error&) {
    }
    return {};
}

std::string manifest_version(const std::filesystem::path& directory)
{
    try {
        const kpl::Plugin parsed = kpl::parse(fs::read_text(directory / "plugin.kpl"),
                                              (directory / "plugin.kpl").string());
        if (parsed.manifest) {
            for (const kpl::Statement& statement : parsed.manifest->statements) {
                if (statement.kind == kpl::Statement::Kind::Assignment &&
                    statement.name == "version" && !statement.expressions.empty() &&
                    statement.expressions.front().kind == kpl::Expr::Kind::String)
                    return statement.expressions.front().token.text;
            }
        }
    }
    catch (const diag::Error&) {
    }
    return "0.0.0";
}

} // namespace

std::string declared_version(const std::filesystem::path& plugin_directory)
{
    return manifest_version(plugin_directory);
}

namespace
{

// Is this argument a git remote rather than a name or a path?
//
// `file://` is included because it is how a local clone is tested and how a
// mirror on a shared filesystem is referenced — git treats it as a real
// remote, and leaving it out would send it down the "local directory" branch
// and copy the working tree including its .git.
bool looks_like_git_url(const std::string& source)
{
    return source.starts_with("http://") || source.starts_with("https://") ||
           source.starts_with("git@") || source.starts_with("ssh://") ||
           source.starts_with("git://") || source.starts_with("file://") ||
           source.ends_with(".git");
}

// Where a plugin should be installed.
std::filesystem::path install_dir(const InstallRequest& request, const std::string& name)
{
    if (request.project_local)
        return request.project_root / ".kap" / "plugins" / name;
    const std::filesystem::path user = paths::user_plugin_dir();
    if (user.empty()) {
        throw diag::Error{diag::error(
            "cannot work out where to install plugins",
            {},
            {"neither $XDG_DATA_HOME nor $HOME is set",
             "use 'kap plugin install --project <source>' to install into this project instead"})};
    }
    return user / name;
}

// A scratch directory under ~/.cache/kap/plugins-src (§6.4), or the system temp
// directory when there is no cache home. Ephemeral by contract.
std::filesystem::path scratch_dir(const std::string& name)
{
    std::filesystem::path base = paths::plugin_source_cache_dir();
    if (base.empty())
        base = std::filesystem::temp_directory_path() / "kap-plugins-src";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    const std::filesystem::path scratch = base / (name + ".staging");
    std::filesystem::remove_all(scratch, ec);
    return scratch;
}

} // namespace

// --- index -------------------------------------------------------------------------------

const Entry* Index::find(const std::string& name) const
{
    const auto found = plugins.find(name);
    return found == plugins.end() ? nullptr : &found->second;
}

const Bundle* Index::find_bundle(const std::string& name) const
{
    const auto found = bundles.find(name);
    return found == bundles.end() ? nullptr : &found->second;
}

Index parse_index(const toml::Document& document, const std::filesystem::path& file)
{
    Index index;
    index.file = file;

    const toml::Value& root = document.root();

    if (const toml::Value* plugins = field(root, "plugins"); plugins != nullptr) {
        if (plugins->kind != toml::Value::Kind::Table) {
            throw diag::Error{diag::error("[plugins] must be a table of plugin entries",
                                          diag::Location{file.string()})};
        }
        for (const auto& [name, table] : plugins->table) {
            if (table.kind != toml::Value::Kind::Table)
                continue;
            Entry entry;
            entry.name           = name;
            entry.description    = string_field(table, "description");
            entry.version        = string_field(table, "version", "0.0.0");
            entry.url            = string_field(table, "url");
            entry.ref            = string_field(table, "ref");
            entry.subdir         = string_field(table, "subdir");
            entry.checksum       = string_field(table, "checksum");
            entry.tags           = string_list_field(table, "tags");
            entry.install_script = string_field(table, "install_script");
            if (entry.url.empty() && entry.install_script.empty()) {
                throw diag::Error{
                    diag::error("registry entry '" + name +
                                    "' has neither 'url' nor "
                                    "'install_script'",
                                diag::Location{file.string()},
                                {"an entry needs one of: a git remote in 'url', or an "
                                 "installer script URL in 'install_script'"})};
            }
            index.plugins.emplace(name, std::move(entry));
        }
    }

    if (const toml::Value* bundles = field(root, "bundles"); bundles != nullptr) {
        if (bundles->kind != toml::Value::Kind::Table) {
            throw diag::Error{diag::error("[bundles] must be a table of bundle entries",
                                          diag::Location{file.string()})};
        }
        for (const auto& [name, table] : bundles->table) {
            if (table.kind != toml::Value::Kind::Table)
                continue;
            Bundle bundle;
            bundle.name        = name;
            bundle.description = string_field(table, "description");
            bundle.plugins     = string_list_field(table, "plugins");
            index.bundles.emplace(name, std::move(bundle));
        }
    }

    return index;
}

std::filesystem::path index_file(const std::filesystem::path& project_root)
{
    const std::string override_value = paths::env_or_empty("KAP_REGISTRY");
    if (!override_value.empty())
        return std::filesystem::path(override_value);

    std::vector<std::filesystem::path> candidates;
    if (const std::filesystem::path user = paths::registry_dir(); !user.empty())
        candidates.push_back(user / "index.toml");
    if (!project_root.empty())
        candidates.push_back(project_root / "registry" / "index.toml");
    if (const std::filesystem::path bundled = paths::bundled_plugin_dir(); !bundled.empty())
        candidates.push_back(bundled.parent_path() / "registry" / "index.toml");

    for (const std::filesystem::path& candidate : candidates)
        if (fs::is_file(candidate))
            return candidate;
    return {};
}

std::optional<Index> load_index(const std::filesystem::path& project_root)
{
    const std::filesystem::path file = index_file(project_root);
    if (!file.empty() && fs::is_file(file))
        return parse_index(toml::parse(fs::read_text(file), file.string()), file);

    // Nothing on disk. A build with plugins compiled in carries the index too,
    // which is what lets `kap plugin search` and `kap plugin install --bundle`
    // work on a binary that has never seen a file — the case that used to dead
    // end with "no registry index found" and no way forward.
    if (const std::string_view embedded = bundled::registry_index(); !embedded.empty()) {
        return parse_index(toml::parse(embedded, "<embedded registry>"), "<embedded registry>");
    }
    return std::nullopt;
}

// --- lockfile ------------------------------------------------------------------------------

const char* origin_name(Origin origin)
{
    switch (origin) {
        case Origin::Registry:
            return "registry";
        case Origin::Git:
            return "git";
        case Origin::Script:
            return "script";
        case Origin::Embedded:
            return "embedded";
        case Origin::Link:
            return "link";
        case Origin::Local:
            return "local";
    }
    return "unknown";
}

namespace
{

Origin origin_from_name(const std::string& name)
{
    if (name == "git")
        return Origin::Git;
    if (name == "script")
        return Origin::Script;
    if (name == "embedded")
        return Origin::Embedded;
    if (name == "link")
        return Origin::Link;
    if (name == "local")
        return Origin::Local;
    return Origin::Registry;
}

} // namespace

Lockfile load_lockfile(const std::filesystem::path& file)
{
    Lockfile lock;
    lock.file = file;
    if (file.empty() || !fs::is_file(file))
        return lock;

    // A lockfile kap cannot read is treated as empty rather than fatal. It is
    // derived state that install/remove rewrite anyway, and refusing to run
    // `kap build` because a bookkeeping file got corrupted would be a poor
    // trade — the plugins themselves are still on disk and still discoverable.
    toml::Document document;
    try {
        document = toml::parse(fs::read_text(file), file.string());
    }
    catch (const diag::Error&) {
        return lock;
    }

    const toml::Value* plugins = field(document.root(), "plugins");
    if (plugins == nullptr || plugins->kind != toml::Value::Kind::Table)
        return lock;

    for (const auto& [name, table] : plugins->table) {
        if (table.kind != toml::Value::Kind::Table)
            continue;
        Installed installed;
        installed.name      = name;
        installed.version   = string_field(table, "version", "0.0.0");
        installed.origin    = origin_from_name(string_field(table, "origin", "registry"));
        installed.url       = string_field(table, "url");
        installed.ref       = string_field(table, "ref");
        installed.subdir    = string_field(table, "subdir");
        installed.checksum  = string_field(table, "checksum");
        installed.enabled   = boolean_field(table, "enabled", true);
        installed.pinned    = string_field(table, "pinned");
        installed.directory = string_field(table, "directory");
        lock.plugins.emplace(name, std::move(installed));
    }
    return lock;
}

void save_lockfile(const Lockfile& lock)
{
    if (lock.file.empty())
        return;

    toml::Value root    = toml::make_table();
    toml::Value plugins = toml::make_table();
    for (const auto& [name, installed] : lock.plugins) {
        toml::Value row        = toml::make_table();
        row.table["checksum"]  = toml::make_string(installed.checksum);
        row.table["directory"] = toml::make_string(installed.directory);
        row.table["enabled"]   = toml::make_boolean(installed.enabled);
        row.table["origin"]    = toml::make_string(origin_name(installed.origin));
        row.table["pinned"]    = toml::make_string(installed.pinned);
        row.table["ref"]       = toml::make_string(installed.ref);
        row.table["subdir"]    = toml::make_string(installed.subdir);
        row.table["url"]       = toml::make_string(installed.url);
        row.table["version"]   = toml::make_string(installed.version);
        plugins.table[name]    = std::move(row);
    }
    root.table["plugins"] = std::move(plugins);

    std::error_code ec;
    if (!lock.file.parent_path().empty())
        std::filesystem::create_directories(lock.file.parent_path(), ec);

    // Write-then-rename: a lockfile truncated by an interrupted write would
    // make every plugin look uninstalled.
    const std::filesystem::path temp = lock.file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out << "# installed-plugins.toml — written by kap (design doc §6.4).\n"
               "# Records where each installed plugin came from. Edit with\n"
               "# 'kap plugin ...' rather than by hand.\n\n";
        out << toml::write(root);
        if (!out)
            return;
    }
    std::filesystem::rename(temp, lock.file, ec);
    if (ec)
        std::filesystem::remove(temp, ec);
}

void apply_lockfile(const Lockfile& lock, std::vector<plugin::Located>& plugins)
{
    for (plugin::Located& located : plugins) {
        const auto found = lock.plugins.find(located.name);
        if (found != lock.plugins.end())
            located.enabled = found->second.enabled;
    }
}

// --- fetching ---------------------------------------------------------------------------------

std::string git_fetch(const std::string&           url,
                      const std::string&           ref,
                      const std::filesystem::path& destination,
                      bool                         verbose)
{
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);

    exec::Options options;
    options.root    = destination.parent_path();
    options.verbose = verbose;
    options.color   = false;

    // A full 40-character hex SHA cannot be cloned with --branch: git only
    // accepts a branch or tag name there. Fetching the object explicitly is
    // the portable way to pin a commit, and it stays shallow.
    const bool is_commit_sha =
        ref.size() == 40 && std::all_of(ref.begin(), ref.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });

    const auto run_one = [&](std::vector<std::string>          command,
                             const std::optional<std::string>& cwd) -> bool {
        kpl::CommandSpec spec;
        kpl::Step        step;
        step.command = std::move(command);
        step.cwd     = cwd;
        spec.steps.push_back(std::move(step));
        return exec::run(spec, options).ok();
    };

    if (is_commit_sha) {
        const std::string leaf = destination.filename().string();
        if (!run_one({"git", "init", "--quiet", leaf}, std::nullopt))
            return "git init failed";
        if (!run_one({"git", "remote", "add", "origin", url}, leaf))
            return "git remote add failed";
        if (!run_one({"git", "fetch", "--quiet", "--depth", "1", "origin", ref}, leaf))
            return "git fetch of '" + ref + "' failed";
        if (!run_one({"git", "checkout", "--quiet", "FETCH_HEAD"}, leaf))
            return "git checkout failed";
        return {};
    }

    std::vector<std::string> command = {"git", "clone", "--quiet", "--depth", "1"};
    if (!ref.empty()) {
        command.push_back("--branch");
        command.push_back(ref);
    }
    command.push_back(url);
    command.push_back(destination.string());
    if (!run_one(std::move(command), std::nullopt)) {
        return ref.empty() ? "git clone of " + url + " failed"
                           : "git clone of " + url + " at '" + ref + "' failed";
    }
    return {};
}

// Is `program` on PATH? The same access(X_OK) scan `project.tool()` uses, and
// for the same reason: it answers the question without running anything.
bool on_path(const std::string& program)
{
    const std::string path = paths::env_or_empty("PATH");
    if (path.empty())
        return false;
    for (const std::filesystem::path& dir : paths::split_path_list(path)) {
        if (::access((dir / program).c_str(), X_OK) == 0)
            return true;
    }
    return false;
}

// Is this a URL kap is willing to download from?
//
// HTTPS only, with one exception: plain HTTP to loopback. kap *executes* what
// it downloads from an installer-script URL, and a script fetched over plain
// HTTP can be replaced by anyone on the path between you and the server.
// Loopback is exempt because there is no such path — and because forbidding it
// would leave no way to test this code without a certificate authority.
std::string url_policy_error(const std::string& url)
{
    if (url.starts_with("https://"))
        return {};
    if (url.starts_with("http://")) {
        const std::string rest = url.substr(std::string("http://").size());
        if (rest.starts_with("127.0.0.1") || rest.starts_with("localhost") ||
            rest.starts_with("[::1]"))
            return {};
        return "refusing to download over plain http: " + url +
               "\n      note: kap runs what it downloads from an installer URL, and plain "
               "http can be tampered with in transit"
               "\n      note: use an https:// address, or install from a local path";
    }
    return "not an address kap can download: " + url + "\n      note: expected https://";
}

std::string
http_fetch(const std::string& url, const std::filesystem::path& destination, bool verbose)
{
    if (const std::string refusal = url_policy_error(url); !refusal.empty())
        return refusal;

    // Decide which downloader to use *before* running one, rather than trying
    // curl and falling back when it fails. A fallback chain prints the first
    // tool's error even when the second succeeds, which is exactly how this
    // first behaved: an install that worked perfectly still emitted a scary
    // "Protocol not supported" line from curl.
    std::vector<std::string> command;
    if (on_path("curl")) {
        // --fail is not optional: without it curl writes a 404 page to the
        // output file and exits 0, and kap goes on to "validate" HTML.
        command = {"curl", "-fsSL", "-o", destination.string(), url};
    } else if (on_path("wget")) {
        command = {"wget", "-q", "-O", destination.string(), url};
    } else {
        return "cannot download " + url +
               ": neither curl nor wget is installed"
               "\n      note: kap has no HTTP client of its own — design doc §9 permits no "
               "linked TLS library";
    }

    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);

    exec::Options options;
    options.root    = destination.parent_path();
    options.verbose = verbose;
    options.color   = false;

    kpl::CommandSpec spec;
    kpl::Step        step;
    step.command = std::move(command);
    spec.steps.push_back(std::move(step));

    if (!exec::run(spec, options).ok()) {
        std::filesystem::remove(destination, ec);
        return "could not download " + url;
    }
    return {};
}

bool looks_like_install_script(const std::string& source)
{
    const bool remote = source.starts_with("https://") || source.starts_with("http://");
    return remote && (source.ends_with(".sh") || source.ends_with("/install"));
}

std::string run_install_script(const std::filesystem::path& script,
                               const std::filesystem::path& staging,
                               const std::string&           plugin_name,
                               bool                         verbose)
{
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec)
        return "cannot create " + staging.string();

    exec::Options options;
    options.root    = staging;
    options.verbose = verbose;
    options.color   = false;

    kpl::CommandSpec spec;
    kpl::Step        step;
    // `sh <script>`, not `chmod +x` and execute: the file came off the network
    // and there is no reason to make it executable on the user's disk.
    step.command                        = {"/bin/sh", script.string()};
    step.environment["KAP_PLUGIN_DEST"] = staging.string();
    step.environment["KAP_PLUGIN_NAME"] = plugin_name;
    step.environment["KAP_VERSION"]     = kVersionString;
    spec.steps.push_back(std::move(step));

    const exec::Outcome outcome = exec::run(spec, options);
    if (!outcome.ok()) {
        return "the installer script failed with exit status " + std::to_string(outcome.exit_code);
    }
    return {};
}

std::vector<std::string> validate_payload(const std::filesystem::path& directory)
{
    std::vector<std::string> problems;

    const std::filesystem::path manifest = directory / "plugin.kpl";
    if (!fs::is_file(manifest)) {
        problems.push_back("no plugin.kpl in " + directory.string());
        return problems;
    }

    // A parse error's whole value is its position. Carrying only
    // `diagnostic().message` would turn "plugin.kpl:2:14: expected '}'" into
    // "expected '}'", which tells a plugin author nothing about where to look.
    const auto with_location = [](const diag::Error& error) {
        const diag::Location& location = error.diagnostic().location;
        if (location.file.empty() && !location.has_position())
            return error.diagnostic().message;
        return diag::render_location(location) + ": " + error.diagnostic().message;
    };

    kpl::Plugin parsed;
    try {
        parsed = kpl::parse(fs::read_text(manifest), manifest.string());
    }
    catch (const diag::Error& error) {
        problems.push_back(with_location(error));
        return problems;
    }

    // §6.3 step 3: manifest fields present, api_version supported, every
    // declared command type-checks.
    for (std::string& error : kpl::validate(parsed, detect::kSupportedApiVersion))
        problems.push_back(std::move(error));
    for (std::string& error : kpl::type_check(parsed))
        problems.push_back(std::move(error));

    // The detect block has its own well-formedness rules that type_check does
    // not cover, and a plugin whose detect rules are malformed can never match.
    try {
        (void) detect::compile(parsed, directory.filename().string());
    }
    catch (const diag::Error& error) {
        problems.push_back(with_location(error));
    }

    return problems;
}

// --- install ----------------------------------------------------------------------------------

namespace
{

// Stage a payload into `staging` from whatever the source turns out to be, and
// report where inside it the plugin lives.
struct Staged
{
    bool                  ok = false;
    std::string           error;
    std::filesystem::path directory; // the plugin directory inside the staging area
    Origin                origin = Origin::Registry;
    std::string           url;
    std::string           ref;
    std::string           subdir;
    std::string           expected_checksum;

    // Set when the payload comes from an installer script that has been
    // downloaded but not yet run. Running it is deferred until after the user
    // confirms, because executing a script off the network is the one step
    // here that declining afterwards cannot undo.
    std::filesystem::path script;
    bool                  needs_script_confirmation = false;
};

Staged stage(const InstallRequest&        request,
             const std::optional<Index>&  index,
             const std::filesystem::path& staging,
             bool                         verbose)
{
    Staged staged;

    // 0. A first-party plugin compiled into this binary.
    //
    // Checked before the registry so `kap plugin install cmake-cpp` works
    // offline and instantly on an embedded build. It is also the honest order:
    // the embedded copy *is* what this binary ships, so fetching possibly
    // different text over the network would be the surprising choice.
    if (!request.link) {
        if (const bundled::Plugin* embedded = bundled::find(request.source); embedded != nullptr) {
            staged.origin = Origin::Embedded;
            staged.url    = "compiled into kap " + std::string(kVersionString);
            if (const std::string error = bundled::materialize(*embedded, staging);
                !error.empty()) {
                staged.error = error;
                return staged;
            }
            staged.directory = staging;
            staged.ok        = true;
            return staged;
        }
    }

    // 1. A registry name.
    if (index && index->find(request.source) != nullptr && !request.link) {
        const Entry& entry       = *index->find(request.source);
        staged.origin            = Origin::Registry;
        staged.ref               = entry.ref;
        staged.subdir            = entry.subdir;
        staged.expected_checksum = entry.checksum;

        // An installer script is preferred over a git remote when the entry
        // offers both: it needs only curl or wget, where the git path needs a
        // working git *and* a clone of the whole repository.
        if (!entry.install_script.empty()) {
            staged.url                         = entry.install_script;
            const std::filesystem::path script = staging.string() + ".install.sh";
            if (const std::string error = http_fetch(entry.install_script, script, verbose);
                !error.empty()) {
                staged.error = error;
                return staged;
            }
            staged.script                    = script;
            staged.needs_script_confirmation = true;
            staged.directory                 = staging;
            staged.ok                        = true;
            return staged;
        }

        staged.url = entry.url;
        if (const std::string error = git_fetch(entry.url, entry.ref, staging, verbose);
            !error.empty()) {
            staged.error = error;
            return staged;
        }
        staged.directory = entry.subdir.empty() ? staging : staging / entry.subdir;
        staged.ok        = true;
        return staged;
    }

    // 2. An installer-script URL, given directly:
    //        kap plugin install https://example.com/kap-zig/install.sh
    if (looks_like_install_script(request.source)) {
        staged.origin                      = Origin::Script;
        staged.url                         = request.source;
        const std::filesystem::path script = staging.string() + ".install.sh";
        if (const std::string error = http_fetch(request.source, script, verbose); !error.empty()) {
            staged.error = error;
            return staged;
        }
        staged.script                    = script;
        staged.needs_script_confirmation = true;
        staged.directory                 = staging;
        staged.ok                        = true;
        return staged;
    }

    // 3. A git URL.
    if (looks_like_git_url(request.source)) {
        staged.origin = Origin::Git;
        staged.url    = request.source;
        if (const std::string error = git_fetch(request.source, {}, staging, verbose);
            !error.empty()) {
            staged.error = error;
            return staged;
        }
        staged.directory = staging;
        staged.ok        = true;
        return staged;
    }

    // 4. A local path.
    std::filesystem::path local(request.source);
    if (local.is_relative())
        local = request.project_root / local;
    if (fs::is_dir(local)) {
        staged.origin    = request.link ? Origin::Link : Origin::Local;
        staged.url       = std::filesystem::weakly_canonical(local).string();
        staged.directory = local;
        staged.ok        = true;
        return staged;
    }

    staged.error = "cannot resolve '" + request.source + "'";
    return staged;
}

} // namespace

InstallResult install(const InstallRequest&                          request,
                      Lockfile&                                      lock,
                      const std::optional<Index>&                    index,
                      const std::function<bool(const std::string&)>& confirm,
                      bool                                           verbose)
{
    InstallResult result;

    const std::filesystem::path staging = scratch_dir("install");

    // The staging area is scratch space; leaving it behind after any exit path
    // would slowly fill ~/.cache with abandoned clones.
    //
    // A downloaded installer script is treated differently on purpose: it is
    // removed when the install *succeeded*, and kept when it did not. When
    // something goes wrong with a script off the network, the script is the
    // first thing anyone will want to look at, and its path is already in the
    // message kap printed.
    struct Cleanup
    {
        std::filesystem::path staging;
        std::filesystem::path script;
        bool                  succeeded = false;

        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove_all(staging, ec);
            if (succeeded && !script.empty())
                std::filesystem::remove(script, ec);
        }
    } cleanup{staging, {}, false};

    // --- §6.3 steps 1-2: resolve the source and fetch the payload.
    const Staged staged = stage(request, index, staging, verbose);
    cleanup.script      = staged.script;
    if (!staged.ok) {
        result.message = staged.error;
        return result;
    }

    // --- An installer script is confirmed BEFORE it runs.
    //
    // Every other source is inert until kap decides to install it, so the
    // ordinary confirmation below — which happens after validation, when kap
    // can describe exactly what it found — is the right place to ask. A script
    // is not inert: running it *is* the irreversible step, and asking
    // afterwards would be asking about something that already happened.
    //
    // The digest is shown because it is the only thing a user can act on: the
    // script is on disk at a path they can read before answering.
    if (staged.needs_script_confirmation) {
        std::string digest = "(could not be read)";
        std::string size   = "?";
        try {
            const std::string text = fs::read_text(staged.script);
            digest                 = sha256::hex(text);
            size                   = std::to_string(text.size()) + " bytes";
        }
        catch (const diag::Error&) {
        }

        const std::string summary =
            "run an installer script from the network\n"
            "  url:      " +
            staged.url + "\n" + "  size:     " + size + "\n" + "  sha256:   " + digest + "\n" +
            "  saved at: " + staged.script.string() + "\n" +
            "\n"
            "  This script runs as you, with your permissions. Read it before saying\n"
            "  yes if you did not write it. kap will still validate whatever it\n"
            "  produces before installing anything.\n";

        if (!confirm(summary)) {
            result.message = "cancelled";
            return result;
        }

        if (const std::string error =
                run_install_script(staged.script, staging, request.source, verbose);
            !error.empty()) {
            result.message = error;
            return result;
        }
    }

    // --- §6.3 step 3: validate before anything is written anywhere.
    const std::vector<std::string> problems = validate_payload(staged.directory);
    if (!problems.empty()) {
        std::string message = "the plugin at '" + request.source + "' is not usable:";
        for (const std::string& problem : problems)
            message += "\n      note: " + problem;
        result.message = message;
        return result;
    }

    std::string name = manifest_name(staged.directory);
    if (name.empty())
        name = staged.directory.filename().string();
    const std::string version = manifest_version(staged.directory);

    // The checksum gate (§6.3 step 3, §12 Q3). Only registry installs carry an
    // expected digest — a git URL the user typed themselves has nobody to
    // compare against — and when one is present it is enforced, not warned
    // about. A mismatch is either a compromised mirror or a stale index; both
    // want the install to stop.
    const std::optional<std::string> actual_checksum = hash_directory(staged.directory);
    if (!staged.expected_checksum.empty()) {
        if (!actual_checksum) {
            result.message = "cannot checksum the payload for '" + name + "'";
            return result;
        }
        if (*actual_checksum != staged.expected_checksum) {
            result.message = "checksum mismatch for '" + name +
                             "'\n"
                             "      note: index expects " +
                             staged.expected_checksum + "\n" + "      note: payload is    " +
                             *actual_checksum +
                             "\n"
                             "      note: refusing to install; the registry index may be stale, "
                             "or the payload may have been tampered with";
            return result;
        }
    }

    const std::filesystem::path destination = install_dir(request, name);

    if (fs::exists(destination) && !request.force) {
        const auto existing = lock.plugins.find(name);
        if (existing != lock.plugins.end() && existing->second.version == version) {
            result.name    = name;
            result.version = version;
            result.message = "'" + name + "' " + version + " is already installed";
            return result;
        }
    }

    // --- §7: print a summary and require confirmation unless --yes.
    std::string summary = "install '" + name + "' " + version + "\n" +
                          "  source:      " + origin_name(staged.origin) + " " + staged.url + "\n";
    if (!staged.ref.empty())
        summary += "  ref:         " + staged.ref + "\n";
    if (!staged.subdir.empty())
        summary += "  subdirectory: " + staged.subdir + "\n";
    summary += "  destination: " + destination.string() + "\n";
    if (request.link)
        summary += "  mode:        symlink (edits to the source take effect immediately)\n";
    if (!staged.expected_checksum.empty())
        summary += "  checksum:    verified\n";
    else if (staged.origin == Origin::Embedded)
        summary += "  checksum:    not needed — this payload came out of the binary\n";
    else
        summary += "  checksum:    none in the registry — kap cannot verify this payload\n";

    // A script install has already been confirmed, at the only moment where
    // saying no could still prevent anything: before the script ran.
    if (!staged.needs_script_confirmation && !confirm(summary)) {
        result.message = "cancelled";
        return result;
    }

    // --- §6.3 step 4: atomic install.
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);

    if (request.link) {
        // A symlink is the point of --link: the user is developing the plugin
        // and wants their edits live, so copying would defeat it.
        std::filesystem::remove_all(destination, ec);
        std::filesystem::create_directory_symlink(
            std::filesystem::weakly_canonical(staged.directory), destination, ec);
        if (ec) {
            result.message = "cannot create a symlink at " + destination.string();
            return result;
        }
    } else {
        // Copy beside the destination and rename into place, so a failure
        // partway through cannot leave a half-installed plugin that discovery
        // would then find and try to run.
        const std::filesystem::path incoming = destination.string() + ".incoming";
        std::filesystem::remove_all(incoming, ec);
        if (!copy_plugin(staged.directory, incoming)) {
            std::filesystem::remove_all(incoming, ec);
            result.message = "cannot copy the plugin into " + destination.string();
            return result;
        }
        std::filesystem::remove_all(destination, ec);
        std::filesystem::rename(incoming, destination, ec);
        if (ec) {
            std::filesystem::remove_all(incoming, ec);
            result.message = "cannot move the plugin into place at " + destination.string();
            return result;
        }
    }

    // --- §6.3 step 5: the lockfile.
    Installed installed;
    installed.name      = name;
    installed.version   = version;
    installed.origin    = staged.origin;
    installed.url       = staged.url;
    installed.ref       = staged.ref;
    installed.subdir    = staged.subdir;
    installed.checksum  = actual_checksum.value_or("");
    installed.directory = destination.string();
    if (const auto previous = lock.plugins.find(name); previous != lock.plugins.end()) {
        // Reinstalling must not silently re-enable a plugin the user disabled,
        // or drop a pin they set.
        installed.enabled = previous->second.enabled;
        installed.pinned  = previous->second.pinned;
    }
    lock.plugins[name] = std::move(installed);
    save_lockfile(lock);

    // --- §6.3 step 7: invalidate the detection cache. The set of candidates
    // just changed, so every cached resolution on this machine may be wrong.
    if (!request.project_root.empty())
        detect::invalidate_cache(request.project_root);

    cleanup.succeeded = true;
    result.installed  = true;
    result.name       = name;
    result.version    = version;
    result.origin     = staged.origin;
    result.directory  = destination;
    return result;
}

InstallResult remove(const std::string&           name,
                     Lockfile&                    lock,
                     const std::filesystem::path& project_root,
                     bool                         verbose)
{
    (void) verbose;
    InstallResult result;
    result.name = name;

    const auto found = lock.plugins.find(name);
    if (found == lock.plugins.end()) {
        result.message = "'" + name + "' is not installed";
        return result;
    }

    std::error_code             ec;
    const std::filesystem::path directory(found->second.directory);
    if (!directory.empty()) {
        // remove_all on a symlink removes the link, not its target — which is
        // exactly right for a --link install: uninstalling must never delete
        // the working copy the user is developing in.
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, ec)))
            std::filesystem::remove(directory, ec);
        else
            std::filesystem::remove_all(directory, ec);
    }

    result.origin = found->second.origin;
    lock.plugins.erase(found);
    save_lockfile(lock);

    if (!project_root.empty())
        detect::invalidate_cache(project_root);

    result.installed = true;
    return result;
}

InstallResult update(const std::string&           name,
                     Lockfile&                    lock,
                     const std::optional<Index>&  index,
                     const std::filesystem::path& project_root,
                     bool                         verbose)
{
    InstallResult result;
    result.name = name;

    const auto found = lock.plugins.find(name);
    if (found == lock.plugins.end()) {
        result.message = "'" + name + "' is not installed";
        return result;
    }
    if (!found->second.pinned.empty()) {
        result.message = "'" + name + "' is pinned to " + found->second.pinned +
                         "; run 'kap plugin pin " + name + " --clear' to unpin it";
        return result;
    }
    if (found->second.origin == Origin::Link) {
        // A symlinked plugin is whatever its working copy currently says; there
        // is nothing to re-fetch, and re-installing would only replace a link
        // with an identical one.
        result.message =
            "'" + name + "' is linked to " + found->second.url + " and is always current";
        return result;
    }

    InstallRequest request;
    request.source        = found->second.origin == Origin::Registry ? name : found->second.url;
    request.project_root  = project_root;
    request.assume_yes    = true;
    request.force         = true;
    request.project_local = std::filesystem::path(found->second.directory)
                                .string()
                                .starts_with((project_root / ".kap").string());

    return install(request, lock, index, [](const std::string&) { return true; }, verbose);
}

// --- scaffolding (§6.1 `kap plugin new`) ------------------------------------------------------

namespace
{

std::string build_system_template(const std::string& name)
{
    return "// " + name +
           R"( — a kap plugin.
//
// Generated by `kap plugin new`. Everything kap knows about this ecosystem
// lives in this file; the binary itself knows nothing about it.
//
// Try it without installing anything:
//     kap plugin doctor --root .        parse, validate, and type-check
//     kap plugin test )" +
           name + R"(   run the fixture cases under tests/
//     kap plugin install --link .       use it for real, live-edited
//
// The language is documented in docs/plugins.md and docs/design.md §5.

manifest {
  name        = ")" +
           name + R"("
  version     = "0.1.0"
  api_version = 1

  // Higher wins when several plugins claim the same directory. Ecosystem
  // plugins sit around 30-50; a generic fallback should sit well below them.
  priority    = 20

  // true means "run alongside the winner" rather than competing to be it —
  // for a sidecar that adds commands without claiming build/test.
  composable  = false

  // Names of plugins to ignore whenever this one matches.
  supersedes  = []
}

// Which projects this plugin claims. Evaluated by the detection engine, never
// by the interpreter, so keep it to cheap filesystem questions.
//
// Available rules:
//   file_exists      "Makefile"
//   dir_exists       ".git"
//   file_exists_any  ["*.sln", "*.csproj"]
//   file_contains    { path: "package.json", pattern: "\"workspaces\"" }
detect {
  file_exists "CHANGE-ME"
}

// The tools the commands below shell out to. `kap doctor` reports on these.
requires {
  any_of   [echo]
  optional []
}

// Everything a user may override in ~/.config/kap/config.toml or ./kap.toml
// under [plugins.)" +
           name + R"(]. The core validates overrides against these
// declarations before any of this file runs, so a typo fails with a clear
// message rather than a confusing command line.
schema {
  build_dir: str       = "build"
  release:   bool      = false
  extra_args: list<str> = []
}

command build(project, config, extra) {
  // `step` appends to the command spec; it never runs anything itself. The
  // executor is the only thing that spawns a process, which is what makes
  // `kap build --dry-run` able to show you exactly what would happen.
  let flags = if config.release then ["--release"] else []
  step ["echo", "building into", config.build_dir] + flags + config.extra_args + extra
}

command test(project, config, extra) {
  step ["echo", "testing"] + extra
}

command clean(project, config) {
  step rm "-rf" config.build_dir
  report_freed_space
}
)";
}

std::string readme_template(const std::string& name)
{
    return "# " + name + R"(

A [kap](https://github.com/kap-project/kap) plugin.

## What it claims

Edit the `detect` block in `plugin.kpl` — right now it looks for a file called
`CHANGE-ME`, which nothing has.

## Configuration

| Key | Type | Default | Meaning |
|---|---|---|---|
| `build_dir` | `str` | `"build"` | Where build output goes |
| `release` | `bool` | `false` | Build in release mode |
| `extra_args` | `list<str>` | `[]` | Extra arguments for every build |

```toml
# kap.toml
[plugins.)" +
           name + R"(]
build_dir = "out"
release = true
```

## Developing

```sh
kap plugin doctor --root .     # parse, validate, type-check
kap plugin test )" +
           name + R"(       # run the fixture cases below
kap plugin install --link .    # install a live-editable symlink
```

## Tests

`tests/fixtures/<fixture>/` is a fake project tree; each
`tests/expected/<fixture>.<command>.steps.json` is one case, asserting the
exact command list that fixture and command produce. Nothing is executed, so
the tests need no toolchain at all.
)";
}

} // namespace

std::vector<std::string> templates()
{
    return {"build-system"};
}

InstallResult scaffold(const std::string&           name,
                       const std::string&           template_name,
                       const std::filesystem::path& directory)
{
    InstallResult result;
    result.name = name;

    const std::vector<std::string> known = templates();
    if (std::find(known.begin(), known.end(), template_name) == known.end()) {
        std::string available;
        for (const std::string& option : known)
            available += (available.empty() ? "" : ", ") + option;
        result.message = "unknown template '" + template_name + "'; expected one of: " + available;
        return result;
    }

    if (fs::exists(directory)) {
        result.message = directory.string() + " already exists";
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory / "tests" / "fixtures" / "example", ec);
    std::filesystem::create_directories(directory / "tests" / "expected", ec);
    if (ec) {
        result.message = "cannot create " + directory.string();
        return result;
    }

    const auto write_file = [](const std::filesystem::path& path, const std::string& contents) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        return static_cast<bool>(out);
    };

    if (!write_file(directory / "plugin.kpl", build_system_template(name)) ||
        !write_file(directory / "README.md", readme_template(name)) ||
        !write_file(directory / "tests" / "fixtures" / "example" / "CHANGE-ME", "") ||
        // One working test case, so `kap plugin test` says something useful the
        // moment the plugin is created rather than "no test cases".
        !write_file(directory / "tests" / "expected" / "example.build.steps.json",
                    "{\n"
                    "  \"steps\": [\n"
                    "    { \"cmd\": [\"echo\", \"building into\", \"build\"] }\n"
                    "  ]\n"
                    "}\n")) {
        result.message = "cannot write into " + directory.string();
        return result;
    }

    result.installed = true;
    result.directory = directory;
    return result;
}

} // namespace registry
} // namespace kap
