#pragma once

// core/registry.hpp
//
// The plugin manager (design doc §6, Milestone 7): the registry index, the
// install pipeline, and the lockfile that records what is installed.
//
// Three things live here, and they are separate on purpose:
//
//   1. **The index** (§6.2) — a static, git-hosted `index.toml` describing the
//      plugins that exist. No custom server: fetching it is a `git clone`, and
//      searching it is a TOML lookup.
//   2. **The lockfile** (§6.4) — `~/.local/share/kap/installed-plugins.toml`,
//      recording where each installed plugin came from, at what version, and
//      whether it is enabled or pinned. Discovery (core/plugin.hpp) finds
//      plugins on disk; the lockfile is what remembers their *provenance*.
//   3. **The pipeline** (§6.3) — resolve, fetch, validate, install atomically,
//      update the lockfile, invalidate caches.
//
// ## Trust
//
// §12 Q3 asked "checksum per version in index.toml vs. signed index" and this
// is where it is answered: **checksum**, over SHA-256 (core/sha256.hpp), and it
// is *enforced* — an index entry that carries a checksum causes the install to
// fail if the payload does not match, rather than warning and continuing. A
// signed index remains post-v1 (it needs key distribution, which a static git
// repository does not give us for free).
//
// What a checksum does and does not buy is worth being honest about: it proves
// the payload matches what the index author recorded, so a compromised mirror
// or a truncated download is caught. It does not vouch for the plugin's
// behaviour. That is why §7's confirmation prompt exists and why `--yes` is
// something the user has to type.

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/plugin.hpp"
#include "core/toml.hpp"

namespace kap
{
namespace registry
{

// --- the index (§6.2) ---------------------------------------------------------------

// One plugin as the registry describes it.
struct Entry
{
    std::string              name;
    std::string              description;
    std::string              version;
    std::string              url;      // git remote to clone
    std::string              ref;      // tag, branch, or full commit SHA ("" = default branch)
    std::string              subdir;   // where in the repository the plugin lives ("" = root)
    std::string              checksum; // "sha256:<64 hex>", optional but enforced when present
    std::vector<std::string> tags;
};

// A named set of plugins, for `kap plugin install --bundle core` (§6.2).
struct Bundle
{
    std::string              name;
    std::string              description;
    std::vector<std::string> plugins;
};

struct Index
{
    std::filesystem::path         file;
    std::map<std::string, Entry>  plugins;
    std::map<std::string, Bundle> bundles;

    const Entry*  find(const std::string& name) const;
    const Bundle* find_bundle(const std::string& name) const;
};

// Parse an index document. Throws diag::Error on a malformed one — a registry
// kap half-understands is worse than one it refuses.
Index parse_index(const toml::Document& document, const std::filesystem::path& file);

// Where the index is looked for, highest precedence first:
//
//   $KAP_REGISTRY                          explicit override (tests, dev)
//   ~/.local/share/kap/registry/index.toml the fetched copy (§6.4)
//   <project_root>/registry/index.toml     a checkout that carries its own
//   <prefix>/share/kap/registry/index.toml shipped with the binary
//
// Returns an empty path when none exists.
std::filesystem::path index_file(const std::filesystem::path& project_root);

// Load the index, or nullopt when there is none to load.
std::optional<Index> load_index(const std::filesystem::path& project_root);

// --- the lockfile (§6.4) --------------------------------------------------------------

// How a plugin got onto this machine.
enum class Origin
{
    Registry, // resolved through the index
    Git,      // a git URL given directly
    Link,     // `--link`: a symlink to a working copy
    Local,    // copied from a local directory
};

const char* origin_name(Origin origin);

// One row of installed-plugins.toml.
struct Installed
{
    std::string name;
    std::string version;
    Origin      origin = Origin::Registry;
    std::string url;
    std::string ref;
    std::string subdir;
    std::string checksum;

    // §6.1 `kap plugin enable/disable`. Recorded here rather than by moving
    // files, so disabling is reversible and leaves the plugin inspectable.
    bool enabled = true;

    // §6.1 `kap plugin pin <name> <version>`. When set, `kap plugin update`
    // refuses to move this plugin.
    std::string pinned;

    // Where the plugin was installed to, so remove() does not have to guess.
    std::string directory;
};

struct Lockfile
{
    std::filesystem::path            file;
    std::map<std::string, Installed> plugins;
};

// The `version` a plugin's own manifest declares. Used by `kap plugin list`
// for plugins that were never "installed" — a bundled or in-repo plugin has no
// lockfile row, but it still has a version, and printing "-" for it would make
// the two look like different kinds of thing when they are not.
std::string declared_version(const std::filesystem::path& plugin_directory);

Lockfile load_lockfile(const std::filesystem::path& file);
void     save_lockfile(const Lockfile& lock);

// Apply the lockfile's enable/disable state to a discovered plugin list, so
// callers get one list that already knows what is switched off.
void apply_lockfile(const Lockfile& lock, std::vector<plugin::Located>& plugins);

// --- the install pipeline (§6.3) --------------------------------------------------------

// What `install()` was asked to do.
struct InstallRequest
{
    // A registry name, a git URL, or a local path — resolved in that order,
    // which is why an ambiguous argument is disambiguated by `--link` or by
    // being an existing directory.
    std::string source;

    // Install into <project_root>/.kap/plugins rather than the user directory.
    bool project_local = false;

    // `--link`: symlink a working copy instead of copying it (§6.2).
    bool link = false;

    // `--yes`: skip the confirmation §7 requires.
    bool assume_yes = false;

    // Overwrite an existing installation of the same name.
    bool force = false;

    // Where <project_root>/.kap/plugins is, and where a relative local path is
    // resolved from.
    std::filesystem::path project_root;
};

struct InstallResult
{
    bool                  installed = false;
    std::string           name;
    std::string           version;
    Origin                origin = Origin::Registry;
    std::filesystem::path directory;
    std::string           message; // why it did not install, when it did not
};

// Fetch a git repository into `destination` (§6.3 step 2). Shallow, and pinned
// to `ref` when one is given. Returns an empty string on success, or the
// reason it failed.
std::string git_fetch(const std::string&           url,
                      const std::string&           ref,
                      const std::filesystem::path& destination,
                      bool                         verbose);

// Validate a candidate plugin directory (§6.3 step 3): plugin.kpl parses, the
// manifest has the required fields, api_version is supported, and every
// declared command type-checks. Returns the problems found, empty when healthy.
std::vector<std::string> validate_payload(const std::filesystem::path& directory);

// The whole pipeline. `confirm` is called with a human-readable summary before
// anything is written and must return true to proceed (§7); pass a function
// that always returns true only when `--yes` was given.
InstallResult install(const InstallRequest&                          request,
                      Lockfile&                                      lock,
                      const std::optional<Index>&                    index,
                      const std::function<bool(const std::string&)>& confirm,
                      bool                                           verbose);

// Remove an installed plugin: delete its directory and its lockfile row.
InstallResult remove(const std::string&           name,
                     Lockfile&                    lock,
                     const std::filesystem::path& project_root,
                     bool                         verbose);

// Re-run the install pipeline for one already-installed plugin, keeping its
// origin. A pinned plugin is skipped with an explanation (§6.1).
InstallResult update(const std::string&           name,
                     Lockfile&                    lock,
                     const std::optional<Index>&  index,
                     const std::filesystem::path& project_root,
                     bool                         verbose);

// Scaffold a new plugin (§6.1 `kap plugin new`). `template_name` selects the
// starting point; "build-system" is the one §6.1 names.
InstallResult scaffold(const std::string&           name,
                       const std::string&           template_name,
                       const std::filesystem::path& directory);

// The templates `kap plugin new --template` accepts.
std::vector<std::string> templates();

} // namespace registry
} // namespace kap
