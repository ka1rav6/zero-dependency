// core/paths.cpp
//
// The two path helpers that need POSIX rather than just $HOME arithmetic:
// finding the running executable, and deriving the bundled plugin directory
// from it (design doc §6.5, tier three of the override precedence).

#include "core/paths.hpp"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>
#include <vector>

namespace kap
{
namespace paths
{

std::filesystem::path executable_path()
{
    // /proc/self/exe is a symlink to the running binary. readlink() does not
    // NUL-terminate and gives no way to ask for the required size up front, so
    // the standard technique is to grow the buffer until the result no longer
    // fills it exactly — a result that exactly fills the buffer is ambiguous
    // (it may have been truncated).
    //
    // The loop is bounded rather than open-ended: a path longer than 64 KiB is
    // not a real deployment, and an unbounded loop here would be a denial of
    // service on a filesystem that keeps saying "still too small".
    for (std::size_t size = 256; size <= (64u << 10); size *= 2) {
        std::vector<char> buffer(size);
        const ssize_t     written = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (written < 0)
            return {}; // no /proc, or no permission: answer "unknown", not an error
        if (static_cast<std::size_t>(written) < buffer.size())
            return std::filesystem::path(
                std::string(buffer.data(), static_cast<std::size_t>(written)));
    }
    return {};
}

std::filesystem::path bundled_plugin_dir()
{
    // An explicit override wins, which is what makes the directory testable at
    // all: a unit test cannot move the binary, but it can set one variable.
    const std::string override_value = env_or_empty("KAP_BUNDLED_PLUGIN_DIR");
    if (!override_value.empty())
        return std::filesystem::path(override_value);

    const std::filesystem::path exe = executable_path();
    if (exe.empty())
        return {};

    // <prefix>/bin/kap -> <prefix>/share/kap/plugins. The two parent_path()
    // calls strip "kap" and then "bin"; anything that leaves an empty prefix
    // (a binary at the filesystem root) is reported as "no bundled directory".
    const std::filesystem::path prefix = exe.parent_path().parent_path();
    if (prefix.empty())
        return {};
    return prefix / "share" / "kap" / "plugins";
}

} // namespace paths
} // namespace kap
