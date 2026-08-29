// core/bundled.cpp
//
// The parts of core/bundled.hpp that do not depend on whether plugins were
// compiled in, plus the "nothing is embedded" defaults.
//
// The generated file (cmake/embed_plugins.cmake) defines available(),
// plugins(), and registry_index() when -DKAP_EMBED_PLUGINS=ON; the weak
// defaults for those live in bundled_none.cpp, which is compiled instead. Two
// small files rather than one with #ifdefs, so the generated code has no
// preprocessor conditions to get wrong.

#include "core/bundled.hpp"

#include <algorithm>
#include <fstream>

#include "core/paths.hpp"
#include "core/version.hpp"

namespace kap
{
namespace bundled
{

const Plugin* find(std::string_view name)
{
    for (const Plugin& plugin : plugins())
        if (plugin.name == name)
            return &plugin;
    return nullptr;
}

std::string materialize(const Plugin& plugin, const std::filesystem::path& directory)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
        return "cannot create " + directory.string();

    for (const File& file : plugin.files) {
        const std::filesystem::path target =
            directory / std::filesystem::path(std::string(file.path));
        std::filesystem::create_directories(target.parent_path(), ec);

        // Write-then-rename, so a reader never sees a half-written plugin.kpl.
        // Materialization happens during discovery, which means it can race a
        // second kap running at the same time in another terminal.
        const std::filesystem::path temp = target.string() + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out)
                return "cannot write " + target.string();
            out << file.contents;
            if (!out)
                return "cannot write " + target.string();
        }
        std::filesystem::rename(temp, target, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return "cannot replace " + target.string();
        }
    }
    return {};
}

std::filesystem::path cache_directory()
{
    const std::filesystem::path base = paths::cache_dir();
    return base.empty() ? base : base / "embedded";
}

std::filesystem::path ensure_materialized()
{
    if (!available() || plugins().empty())
        return {};

    const std::filesystem::path directory = cache_directory();
    if (directory.empty())
        return {}; // no HOME and no XDG_CACHE_HOME: nowhere to put them

    // The stamp is the whole fast path. Discovery runs on every invocation, and
    // rewriting eight files each time — or even stat-ing them — would be a cost
    // paid forever to guard against a case that only arises when kap itself is
    // upgraded.
    //
    // Keyed on kap's version because that is exactly when the embedded text can
    // have changed: the plugins are compiled into this binary, so they cannot
    // change without a new binary.
    const std::filesystem::path stamp = directory / ".kap-embedded-version";
    std::error_code             ec;
    if (std::filesystem::is_regular_file(stamp, ec)) {
        std::ifstream in(stamp);
        std::string   recorded;
        std::getline(in, recorded);
        if (recorded == kVersionString)
            return directory;
    }

    for (const Plugin& plugin : plugins()) {
        if (!materialize(plugin, directory / std::filesystem::path(std::string(plugin.name)))
                 .empty())
            return {}; // a read-only or full cache: behave as if nothing is embedded
    }

    std::ofstream out(stamp, std::ios::binary | std::ios::trunc);
    out << kVersionString << "\n";
    if (!out)
        return {};

    return directory;
}

} // namespace bundled
} // namespace kap
