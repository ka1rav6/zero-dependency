#pragma once

// core/kapc.hpp
//
// The KPL compilation cache (design doc §5.14). Parsing a plugin.kpl is cheap,
// but kap parses one on *every* invocation, and §5.14 asks for the AST to be
// cached so the hot path never re-lexes text:
//
//     ~/.cache/kap/ast/<key>.kapc
//
// The blob is a plain little-endian byte encoding of the AST — no compression,
// no pointers, no third-party serialization library (§9). Decoding is a
// bounds-checked linear scan, so a truncated or corrupt file produces a
// diagnostic and a re-parse rather than undefined behaviour.
//
// ## Invalidation
//
// A cache entry carries the source path, the source file's size and
// modification time, and two version numbers: kFormatVersion (bumped whenever
// the AST's shape changes, so a cache written by an older kap is never
// misread) and the KPL api_version the entry was compiled for. `load()`
// re-parses whenever any of these disagrees with reality, which covers §5.14's
// "invalidated when plugin.kpl mtime or api_version changes" plus the case
// §5.14 does not mention — kap itself being upgraded.
//
// ## Cache key
//
// §5.14 writes the file name as `<name>@<version>.kapc`. That cannot work as
// stated: the name and version live *inside* plugin.kpl, so finding the cache
// entry would require parsing the very file the cache exists to avoid parsing.
// The key here is therefore `<directory-name>@<hash-of-absolute-path>.kapc` —
// the human-readable half comes from the plugin's directory, which is known
// without reading anything, and the hash disambiguates two plugins of the same
// name installed at different paths (§6.5 explicitly allows a project-local
// plugin to shadow a user-installed one, so this is a real collision, not a
// hypothetical). Once Milestone 7's installed-plugins.toml records each
// plugin's version, that can be folded into the name as well.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/kpl.hpp"

namespace kap
{
namespace kapc
{

// Bump this whenever the encoding or the AST's shape changes. Every existing
// cache entry is then treated as stale, which is exactly right: an entry
// written by a different kap cannot be trusted to decode into the same tree.
inline constexpr std::uint32_t kFormatVersion = 1;

// Encode a parsed plugin. Deterministic: the same AST always produces the same
// bytes, which is what makes the round-trip testable.
std::string encode(const kpl::Plugin& plugin);

// Decode a blob produced by encode(). Throws diag::Error on a truncated,
// corrupt, or wrong-version blob — callers treat that as a cache miss.
kpl::Plugin decode(std::string_view blob);

// The AST cache directory: $XDG_CACHE_HOME/kap/ast, else ~/.cache/kap/ast
// (design doc §6.4). Returns an empty path when neither variable is set, which
// callers read as "caching is unavailable here" rather than an error.
std::filesystem::path cache_directory();

// The cache file for `source` inside `directory`. See the key discussion above.
std::filesystem::path cache_file(const std::filesystem::path& directory,
                                 const std::filesystem::path& source);

// How a plugin was obtained, for `--verbose` and for tests that need to assert
// the cache is actually being used.
enum class Origin
{
    Parsed,     // read and parsed from plugin.kpl
    CacheHit,   // decoded from a valid .kapc
    CacheWrite, // parsed, and a fresh .kapc was written
};

struct Loaded
{
    kpl::Plugin plugin;
    Origin      origin = Origin::Parsed;
};

// Load a plugin, using `directory` as the AST cache.
//
// Any problem with the cache — missing, stale, corrupt, unwritable — falls
// back to parsing the source, because a cache is an optimisation and must
// never be able to turn a working plugin into a broken one. Only a genuine
// error in plugin.kpl itself propagates.
//
// An empty `directory` disables caching entirely (always Origin::Parsed).
Loaded load(const std::filesystem::path& source, const std::filesystem::path& directory);

} // namespace kapc
} // namespace kap
