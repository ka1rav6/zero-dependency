#pragma once

// core/paths.hpp
//
// Every well-known directory kap reads or writes, resolved in one place
// (design doc §6.4 and §5.12). Three families of location, and kap needs all
// three before Milestone 7 is over:
//
//     ~/.config/kap/config.toml            user configuration (§5.12 layer 2)
//     ~/.local/share/kap/plugins/<name>/   user-installed plugins (§6.4)
//     ~/.local/share/kap/installed-plugins.toml   the lockfile (§6.4)
//     ~/.cache/kap/ast/<key>.kapc          compiled KPL ASTs (§5.14)
//     ~/.cache/kap/plugins-src/            ephemeral git clones (§6.4)
//     <prefix>/share/kap/plugins/<name>/   plugins shipped with the binary (§6.5)
//
// Why a header of its own rather than a `getenv` at each call site: the XDG
// fallback chain ($XDG_*_HOME, else $HOME/.something) has to be identical
// everywhere or a plugin installed by `kap plugin install` lands somewhere
// `kap build` does not look. Writing the chain once makes that class of bug
// impossible, and it gives the tests a single seam — set the three XDG
// variables to a scratch directory and the whole program relocates.
//
// An empty path is the "unavailable" answer, never an exception. A machine
// with no $HOME (a minimal container, a daemon) should still be able to run
// `kap build -n` in a directory with a project-local plugin; callers therefore
// check for empty and degrade rather than aborting.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kap
{
namespace paths
{

// Read an environment variable, treating "set but empty" as unset. POSIX
// allows an empty value, and every caller here wants the same answer for both
// (`XDG_CACHE_HOME=` must fall back to ~/.cache, not resolve to "/kap").
inline std::string env_or_empty(const char* name)
{
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? std::string(value) : std::string();
}

inline std::filesystem::path home()
{
    const std::string value = env_or_empty("HOME");
    return value.empty() ? std::filesystem::path{} : std::filesystem::path(value);
}

// The XDG base-directory chain, shared by the three helpers below: an explicit
// $XDG_<X>_HOME wins, otherwise $HOME/<fallback>, otherwise nothing.
inline std::filesystem::path
xdg_base(const char* variable, std::string_view first, std::string_view second)
{
    const std::string explicit_value = env_or_empty(variable);
    if (!explicit_value.empty())
        return std::filesystem::path(explicit_value);
    const std::filesystem::path h = home();
    if (h.empty())
        return {};
    std::filesystem::path result = h / first;
    if (!second.empty())
        result /= second;
    return result;
}

inline std::filesystem::path config_home()
{
    return xdg_base("XDG_CONFIG_HOME", ".config", {});
}

inline std::filesystem::path data_home()
{
    return xdg_base("XDG_DATA_HOME", ".local", "share");
}

inline std::filesystem::path cache_home()
{
    return xdg_base("XDG_CACHE_HOME", ".cache", {});
}

// kap's own subdirectory of each base. Empty in, empty out.
inline std::filesystem::path config_dir()
{
    const std::filesystem::path base = config_home();
    return base.empty() ? base : base / "kap";
}

inline std::filesystem::path data_dir()
{
    const std::filesystem::path base = data_home();
    return base.empty() ? base : base / "kap";
}

inline std::filesystem::path cache_dir()
{
    const std::filesystem::path base = cache_home();
    return base.empty() ? base : base / "kap";
}

// ~/.config/kap/config.toml — the global layer of §5.12's precedence chain.
inline std::filesystem::path user_config_file()
{
    const std::filesystem::path dir = config_dir();
    return dir.empty() ? dir : dir / "config.toml";
}

// ~/.local/share/kap/plugins — where `kap plugin install` puts things (§6.4).
inline std::filesystem::path user_plugin_dir()
{
    const std::filesystem::path dir = data_dir();
    return dir.empty() ? dir : dir / "plugins";
}

// ~/.local/share/kap/installed-plugins.toml — the lockfile (§6.3 step 5).
inline std::filesystem::path lockfile()
{
    const std::filesystem::path dir = data_dir();
    return dir.empty() ? dir : dir / "installed-plugins.toml";
}

// ~/.local/share/kap/registry — the cached registry index (§6.4).
inline std::filesystem::path registry_dir()
{
    const std::filesystem::path dir = data_dir();
    return dir.empty() ? dir : dir / "registry";
}

// ~/.cache/kap/ast — compiled KPL ASTs (§5.14).
inline std::filesystem::path ast_cache_dir()
{
    const std::filesystem::path dir = cache_dir();
    return dir.empty() ? dir : dir / "ast";
}

// ~/.cache/kap/plugins-src — scratch space for git clones (§6.4). Ephemeral by
// contract: anything here may be deleted between runs.
inline std::filesystem::path plugin_source_cache_dir()
{
    const std::filesystem::path dir = cache_dir();
    return dir.empty() ? dir : dir / "plugins-src";
}

// The running executable's own path, used to find the plugins installed
// alongside it. Empty when it cannot be determined, which callers read as "no
// bundled plugin directory" rather than an error.
//
// Declared here and defined in paths.cpp because the only reliable way to ask
// this question on Linux is readlink("/proc/self/exe"), which needs <unistd.h>
// — a POSIX header this otherwise-portable file should not force on every
// translation unit that just wants ~/.config.
std::filesystem::path executable_path();

// <prefix>/share/kap/plugins, derived from the executable's location: a binary
// at <prefix>/bin/kap finds <prefix>/share/kap/plugins. This is the third and
// lowest tier of §6.5's override precedence.
std::filesystem::path bundled_plugin_dir();

// Split a PATH-style, colon-separated variable into its entries, dropping the
// empty ones. Used for $KAP_PLUGIN_PATH (extra plugin search directories) and
// for the $PATH scan behind `project.tool()`.
inline std::vector<std::filesystem::path> split_path_list(std::string_view text)
{
    std::vector<std::filesystem::path> entries;
    std::size_t                        start = 0;
    while (start <= text.size()) {
        const std::size_t colon = text.find(':', start);
        const std::size_t end   = (colon == std::string_view::npos) ? text.size() : colon;
        if (end > start)
            entries.emplace_back(std::string(text.substr(start, end - start)));
        if (colon == std::string_view::npos)
            break;
        start = colon + 1;
    }
    return entries;
}

} // namespace paths
} // namespace kap
