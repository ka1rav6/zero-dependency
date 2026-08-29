#pragma once

// core/bundled.hpp
//
// The first-party plugins, optionally compiled into the binary.
//
// ## The two ways to install kap
//
// **Files on disk** (the default). `cmake --install` — and therefore
// `scripts/install.sh` — writes the plugins to `<prefix>/share/kap/plugins`,
// where discovery finds them as the "bundled" tier (design doc §6.5). This is
// the ordinary packaging story: the plugins are text files a distributor can
// see, diff, and patch.
//
// **Compiled in** (`-DKAP_EMBED_PLUGINS=ON`). The same plugins are generated
// into a C++ source file by cmake/embed_plugins.cmake and linked into the
// binary, which then needs nothing on disk and no network to work at all. Copy
// that one file to a machine and `kap build` works.
//
// The second exists because the first has a failure mode that is invisible
// until it bites: a `kap` binary copied out of a build tree — the most natural
// thing in the world to do — finds no plugins, no registry, and no way to get
// either, and every error it prints is about the project rather than about the
// missing half of the install.
//
// ## How embedded plugins are used
//
// Not by interpreting them from memory. Discovery, the AST cache, `kap plugin
// doctor`, and the fixture runner all take a *path*, and giving them a second
// in-memory code path would double the surface where the two could disagree.
//
// Instead they are written once into `~/.cache/kap/embedded/<name>/` and then
// treated exactly like any other plugin directory. The cache is derived state
// by definition, so writing there is not a surprise; a stale copy is detected
// by a stamp file holding kap's version, and anything installed in a
// higher-precedence tier still shadows it.
//
// In a build without embedding every function here reports "nothing", and the
// bundled-on-disk tier is the only one.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kap
{
namespace bundled
{

// One file of an embedded plugin.
struct File
{
    std::string_view path; // relative, e.g. "plugin.kpl"
    std::string_view contents;
};

struct Plugin
{
    std::string_view  name;
    std::vector<File> files;
};

// True when this build has plugins compiled in.
bool available();

// The embedded plugins, empty in a build without them.
const std::vector<Plugin>& plugins();

// The embedded copy of registry/index.toml, empty in a build without it.
// Lets `kap plugin search` work on a binary that has never seen a file.
std::string_view registry_index();

// The embedded plugin of that name, or nullptr.
const Plugin* find(std::string_view name);

// Write a plugin's files into `directory`, creating it. Returns an empty
// string on success, or the reason it failed.
std::string materialize(const Plugin& plugin, const std::filesystem::path& directory);

// Where materialized plugins live: $XDG_CACHE_HOME/kap/embedded, or
// ~/.cache/kap/embedded. Empty when there is no cache directory at all.
std::filesystem::path cache_directory();

// Make sure every embedded plugin is present and current under
// cache_directory(), and return that directory — or an empty path when there
// is nothing embedded, or nowhere to write.
//
// Cheap on the common path: a stamp file records the kap version the cache was
// written by, and a matching stamp skips all filesystem work. Called once per
// invocation from plugin discovery.
std::filesystem::path ensure_materialized();

} // namespace bundled
} // namespace kap
