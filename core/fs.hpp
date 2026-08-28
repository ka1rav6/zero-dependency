#pragma once

// core/fs.hpp
//
// Thin wrappers over std::filesystem that apply kap's sandboxing rules
// (design doc §7 + Milestone 1). Everything a KPL plugin can see of the disk
// goes through here:
//
//   - reads are capped (1 MiB) so an accidental /dev/zero read cannot hang the
//     process;
//   - globbing is capped (10 000 results) so a wildcard cannot balloon memory;
//   - paths are canonicalized and checked to stay inside a root before use.
//
// Keeping the caps in one place means the security story is auditable from a
// single file instead of being scattered across the interpreter.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "core/diag.hpp"

namespace kap
{
namespace fs
{

// Hard caps from design doc §7. Keep them together so a security review can
// confirm the bounds in seconds.
inline constexpr std::size_t kMaxReadBytes   = 1u << 20; // 1 MiB
inline constexpr std::size_t kMaxGlobResults = 10000;

bool exists(const std::filesystem::path& path);
bool is_dir(const std::filesystem::path& path);
bool is_file(const std::filesystem::path& path);

// Read an entire file as UTF-8 text. Throws diag::Error if the file is
// missing, unreadable, or larger than `max_bytes`.
std::string read_text(const std::filesystem::path& path, std::size_t max_bytes = kMaxReadBytes);

// List the paths directly under `dir` whose file name matches `pattern`
// (`*` matches any run of characters, `?` matches exactly one). Returns paths
// *relative* to `dir`, sorted, capped at kMaxGlobResults. A non-existent or
// unreadable `dir` yields an empty list — call `exists` first to tell the two
// apart.
std::vector<std::string> glob(const std::filesystem::path& dir, std::string_view pattern);

// Canonicalize (resolve symlinks + make absolute) `path` and answer whether it
// still lives under `root`. Used to enforce "plugins can never escape the
// project root" (design doc §7). Returns false on any canonicalization error
// rather than throwing, so lookups never crash on weird filesystems.
bool is_within(const std::filesystem::path& root, const std::filesystem::path& path);

// Matches `pattern` against `text`, where:
//   *  matches any run of characters (including none),
//   ?  matches exactly one character,
//   everything else matches literally.
bool match_wildcard(std::string_view pattern, std::string_view text);

// --- inline implementations ------------------------------------------------------

inline bool exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

inline bool is_dir(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

inline bool is_file(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

inline bool match_wildcard(std::string_view pattern, std::string_view text)
{
    // Iterative single-backtrack matcher.
    //
    // The obvious implementation recurses on every `*`, trying each possible
    // split. That is exponential: a pattern like "*a*a*a*a*b" against a long
    // run of 'a's re-explores the same suffixes over and over, and it can also
    // blow the call stack. Since glob patterns come from plugins (design doc
    // §7 treats plugin input as untrusted), a pathological pattern must not be
    // able to hang or crash kap.
    //
    // The trick: only the MOST RECENT `*` ever needs to be revisited. When a
    // mismatch happens, extending that last star by one character is always at
    // least as good as any earlier choice, so a single remembered position is
    // enough. That makes the algorithm O(pattern * text) worst case and O(n)
    // in practice, with no recursion at all.
    std::size_t p         = 0;                      // cursor into pattern
    std::size_t t         = 0;                      // cursor into text
    std::size_t star      = std::string_view::npos; // index of the last '*' seen
    std::size_t star_text = 0;                      // where that '*' started matching

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p; // literal or single-character match
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            // Remember this star and start it matching the empty string.
            star      = p;
            star_text = t;
            ++p;
        } else if (star != std::string_view::npos) {
            // Mismatch, but a star is available: let it swallow one more
            // character and retry the rest of the pattern from just after it.
            p = star + 1;
            ++star_text;
            t = star_text;
        } else {
            return false; // mismatch with no star to fall back on
        }
    }

    // Text is exhausted; the pattern matches only if what remains is all stars.
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

inline std::vector<std::string> glob(const std::filesystem::path& dir, std::string_view pattern)
{
    std::vector<std::string> results;

    std::error_code                           ec;
    std::filesystem::directory_iterator       it(dir, ec);
    const std::filesystem::directory_iterator end;
    for (; !ec && it != end && results.size() < kMaxGlobResults; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (match_wildcard(pattern, name)) {
            results.push_back(name);
        }
    }

    // Deterministic output: sorted results make plugin `step` lists stable,
    // which keeps dry-runs and tests reproducible.
    std::sort(results.begin(), results.end());
    return results;
}

inline bool is_within(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code             ec;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path canonical_path = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return false;
    }

    // "path is inside root" ⇔ root is a lexicographic prefix of path.
    auto root_it = canonical_root.begin();
    for (const auto& component : canonical_path) {
        if (root_it == canonical_root.end()) {
            return true; // path continues below root
        }
        if (*root_it != component) {
            return false; // a component diverged (../ escape or sibling)
        }
        ++root_it;
    }
    return root_it == canonical_root.end();
}

inline std::string read_text(const std::filesystem::path& path, std::size_t max_bytes)
{
    // A regular file must exist before we try to open it. Checking this first
    // separates "no such file" from "that is a directory" in the error text,
    // which is most of the value of a diagnostic.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        throw diag::Error{diag::error("cannot read '" + path.string() + "': no such file")};
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        throw diag::Error{diag::error("cannot read '" + path.string() + "': not a regular file")};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw diag::Error{diag::error("cannot open '" + path.string() + "' for reading")};
    }

    // The cap is enforced by reading at most max_bytes + 1 bytes and rejecting
    // the file if that extra byte materialises.
    //
    // Enforcing it on the *stream* rather than on a stat() result is what makes
    // it a real bound. stat can disagree with reality in both directions: a
    // character device such as /dev/zero reports a size of zero while yielding
    // bytes forever (the is_regular_file gate above already turns those away,
    // but the check costs nothing and the two together are what the guarantee
    // rests on), and an ordinary file being appended to can grow between the
    // stat and the read. Sizing off stat and then slurping the whole stream —
    // the original implementation — was unbounded in both cases.
    //
    // stat is still used, as a *hint* for the initial allocation. Without it
    // every read of a 200-byte config file would allocate and zero a full
    // megabyte; with it, the common case is one exactly-sized allocation and
    // one read. Because the hint is only an optimisation, a wrong one costs a
    // reallocation rather than correctness.
    std::uintmax_t    hinted = std::filesystem::file_size(path, ec);
    const std::size_t capacity =
        (!ec && hinted < max_bytes) ? static_cast<std::size_t>(hinted) + 1 : max_bytes + 1;

    std::string out;
    out.resize(capacity);

    std::size_t total = 0;
    for (;;) {
        in.read(out.data() + total, static_cast<std::streamsize>(out.size() - total));

        // bad() is a real I/O failure; fail() alone just means we hit EOF
        // before filling the buffer, which is the normal case.
        if (in.bad()) {
            throw diag::Error{diag::error("I/O error while reading '" + path.string() + "'")};
        }
        total += static_cast<std::size_t>(in.gcount());

        if (total < out.size()) {
            break; // short read: end of file
        }
        if (out.size() > max_bytes) {
            break; // buffer already covers the cap probe; stop and report
        }
        // The hint under-reported the size. Grow to the full cap probe and
        // keep reading; clear() drops the eofbit a short read may have set.
        out.resize(max_bytes + 1);
        in.clear();
    }

    if (total > max_bytes) {
        throw diag::Error{diag::error("refusing to read '" + path.string() +
                                      "': file is larger than the " + std::to_string(max_bytes) +
                                      "-byte cap (design doc §7)")};
    }

    out.resize(total);
    return out;
}

} // namespace fs
} // namespace kap