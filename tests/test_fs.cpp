// tests/test_fs.cpp
//
// Unit tests for the sandboxed filesystem helpers (core/fs.hpp). Tests use a
// unique scratch directory under the system temp dir so they never touch the
// repository and never collide with a parallel test run.

#include "core/diag.hpp"
#include "core/fs.hpp"
#include "harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace
{

// A throwaway directory unique to this process; removed at the end of every
// test that creates one.
std::filesystem::path scratch(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_test_" + name + "_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

} // namespace

KAP_TEST("exists/is_dir/is_file tell files, directories, and missing apart")
{
    const std::filesystem::path dir  = scratch("exists");
    const std::filesystem::path file = dir / "a.txt";
    write_file(file, "hi");

    KAP_ASSERT(kap::fs::exists(dir));
    KAP_ASSERT(kap::fs::is_dir(dir));
    KAP_ASSERT(!kap::fs::is_file(dir));

    KAP_ASSERT(kap::fs::is_file(file));
    KAP_ASSERT(!kap::fs::is_dir(file));

    KAP_ASSERT(!kap::fs::exists(dir / "missing"));

    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text returns the exact file contents")
{
    const std::filesystem::path dir  = scratch("read");
    const std::filesystem::path file = dir / "note.txt";
    write_file(file, "hello kap\n");

    KAP_ASSERT_EQ(kap::fs::read_text(file), std::string("hello kap\n"));
    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text rejects files larger than the cap")
{
    const std::filesystem::path dir  = scratch("cap");
    const std::filesystem::path file = dir / "big.txt";
    write_file(file, std::string(100, 'x'));

    KAP_ASSERT_THROWS(kap::diag::Error, kap::fs::read_text(file, /*max_bytes=*/10));
    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text throws on a missing file")
{
    KAP_ASSERT_THROWS(
        kap::diag::Error,
        kap::fs::read_text(std::filesystem::temp_directory_path() / "kap_no_such_file_xyz"));
});

KAP_TEST("glob returns matching names, sorted, relative to the directory")
{
    const std::filesystem::path dir = scratch("glob");
    write_file(dir / "a.c", "");
    write_file(dir / "b.c", "");
    write_file(dir / "README.md", "");

    const std::vector<std::string> hits = kap::fs::glob(dir, "*.c");
    KAP_ASSERT_EQ(hits.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(hits[0], "a.c");
    KAP_ASSERT_EQ(hits[1], "b.c");

    std::filesystem::remove_all(dir);
});

KAP_TEST("glob on a missing directory is empty, not fatal")
{
    const std::vector<std::string> hits =
        kap::fs::glob(std::filesystem::temp_directory_path() / "kap_no_such_dir_xyz", "*.c");
    KAP_ASSERT(hits.empty());
});

KAP_TEST("match_wildcard handles star, question, and literals")
{
    KAP_ASSERT(kap::fs::match_wildcard("*.cpp", "main.cpp"));
    KAP_ASSERT(!kap::fs::match_wildcard("*.cpp", "main.c"));
    KAP_ASSERT(kap::fs::match_wildcard("a?c", "abc"));
    KAP_ASSERT(!kap::fs::match_wildcard("a?c", "ac"));
    KAP_ASSERT(kap::fs::match_wildcard("*", ""));
    KAP_ASSERT(kap::fs::match_wildcard("*", "anything"));
    KAP_ASSERT(kap::fs::match_wildcard("", ""));
    KAP_ASSERT(!kap::fs::match_wildcard("", "x"));
});

KAP_TEST("is_within accepts paths under the root and rejects escapes")
{
    const std::filesystem::path dir    = scratch("within");
    const std::filesystem::path inside = dir / "sub" / "file.txt";
    std::filesystem::create_directories(inside.parent_path());
    write_file(inside, "x");

    KAP_ASSERT(kap::fs::is_within(dir, inside));
    KAP_ASSERT(kap::fs::is_within(dir, dir / "sub"));

    const std::filesystem::path sibling =
        dir.parent_path() / ("kap_neighbour_" + std::to_string(getpid()));
    std::filesystem::remove_all(sibling);
    std::filesystem::create_directories(sibling);
    KAP_ASSERT(!kap::fs::is_within(dir, sibling));

    std::filesystem::remove_all(sibling);
    std::filesystem::remove_all(dir);
});

// --- Wildcard matching: correctness and cost -------------------------------------

KAP_TEST("match_wildcard handles multiple and adjacent stars")
{
    KAP_ASSERT(kap::fs::match_wildcard("*test*", "my_test_file"));
    KAP_ASSERT(kap::fs::match_wildcard("**", "anything"));
    KAP_ASSERT(kap::fs::match_wildcard("a**b", "ab"));
    KAP_ASSERT(kap::fs::match_wildcard("a**b", "axxxb"));
    KAP_ASSERT(kap::fs::match_wildcard("*.tar.gz", "archive.tar.gz"));
    KAP_ASSERT(!kap::fs::match_wildcard("*.tar.gz", "archive.tar.bz2"));
});

KAP_TEST("match_wildcard anchors both ends of the pattern")
{
    // Unlike a substring search, "abc" must match only "abc".
    KAP_ASSERT(kap::fs::match_wildcard("abc", "abc"));
    KAP_ASSERT(!kap::fs::match_wildcard("abc", "xabc"));
    KAP_ASSERT(!kap::fs::match_wildcard("abc", "abcx"));
    KAP_ASSERT(kap::fs::match_wildcard("*abc", "xabc"));
    KAP_ASSERT(kap::fs::match_wildcard("abc*", "abcx"));
});

KAP_TEST("match_wildcard backtracks correctly when a star overshoots")
{
    // The greedy-then-backtrack path: the star must first try to swallow the
    // whole string, then give characters back until the tail fits.
    KAP_ASSERT(kap::fs::match_wildcard("*.xcodeproj", "MyApp.xcodeproj"));
    KAP_ASSERT(kap::fs::match_wildcard("*a*b", "xxaxxb"));
    KAP_ASSERT(!kap::fs::match_wildcard("*a*b", "xxaxx"));
    KAP_ASSERT(kap::fs::match_wildcard("a*a*a", "aaa"));
    KAP_ASSERT(!kap::fs::match_wildcard("a*a*a", "aa"));
});

KAP_TEST("a star can match trailing text after the last literal")
{
    KAP_ASSERT(kap::fs::match_wildcard("Cargo*", "Cargo.toml"));
    KAP_ASSERT(kap::fs::match_wildcard("?*", "x"));
    KAP_ASSERT(!kap::fs::match_wildcard("?*", ""));
});

KAP_TEST("a pathological pattern still matches in linear time")
{
    // Regression guard for the old recursive matcher, which explored every
    // way of splitting the text between stars. At 24 characters it already
    // took ~33 ms; each extra character roughly doubled that, so a plugin
    // could have hung kap with one glob. The iterative matcher is O(n*m).
    //
    // No timing assertion (that would be flaky on a loaded CI box) — the test
    // simply cannot complete at all if the exponential behaviour returns.
    const std::string text(2048, 'a');
    KAP_ASSERT(!kap::fs::match_wildcard("*a*a*a*a*a*a*a*a*a*a*a*a*b", text));
    KAP_ASSERT(kap::fs::match_wildcard("*a*a*a*a*a*a*a*a*a*a*a*a*a", text));
});

KAP_TEST("a deeply starred pattern does not recurse into a stack overflow")
{
    // The old implementation recursed once per star; 10 000 of them would
    // exhaust the stack. The iterative version uses O(1) space.
    const std::string pattern(10000, '*');
    KAP_ASSERT(kap::fs::match_wildcard(pattern, "anything at all"));
});

// --- read_text caps --------------------------------------------------------------

KAP_TEST("read_text enforces its cap on the stream, not on a stat() result")
{
    // /dev/zero reports a size of 0 but yields bytes forever. Sizing the read
    // off stat() and then slurping the stream (the original implementation)
    // never terminates. The cap must be enforced while reading.
    //
    // Guarded by exists() so the test is a no-op on a platform without it.
    if (std::filesystem::exists("/dev/zero")) {
        KAP_ASSERT_THROWS(kap::diag::Error, kap::fs::read_text("/dev/zero", 1024));
    }
});

KAP_TEST("read_text accepts a file of exactly the cap size")
{
    // Off-by-one guard: the cap is a maximum, not an exclusive bound.
    const std::filesystem::path dir  = scratch("exact_cap");
    const std::filesystem::path file = dir / "exact.txt";
    write_file(file, std::string(64, 'x'));

    KAP_ASSERT_EQ(kap::fs::read_text(file, 64).size(), static_cast<std::size_t>(64));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::fs::read_text(file, 63));

    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text reads an empty file as an empty string")
{
    const std::filesystem::path dir  = scratch("empty_read");
    const std::filesystem::path file = dir / "empty.txt";
    write_file(file, "");

    KAP_ASSERT_EQ(kap::fs::read_text(file), "");

    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text preserves embedded NUL bytes and binary content")
{
    // Reads are byte-exact: a length-based read must not stop at a NUL the way
    // a C-string copy would.
    const std::filesystem::path dir     = scratch("binary_read");
    const std::filesystem::path file    = dir / "bin.dat";
    const std::string           payload = std::string("a\0b\0c", 5);
    write_file(file, payload);

    const std::string got = kap::fs::read_text(file);
    KAP_ASSERT_EQ(got.size(), static_cast<std::size_t>(5));
    KAP_ASSERT_EQ(got, payload);

    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text rejects a directory with a distinct message")
{
    const std::filesystem::path dir = scratch("read_dir");
    try {
        (void) kap::fs::read_text(dir);
        KAP_ASSERT(false); // unreachable
    }
    catch (const kap::diag::Error& e) {
        KAP_ASSERT(e.report().find("not a regular file") != std::string::npos);
    }
    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text says 'no such file' for a missing path")
{
    try {
        (void) kap::fs::read_text(std::filesystem::temp_directory_path() /
                                  "kap_definitely_missing");
        KAP_ASSERT(false); // unreachable
    }
    catch (const kap::diag::Error& e) {
        KAP_ASSERT(e.report().find("no such file") != std::string::npos);
    }
});

// --- glob ------------------------------------------------------------------------

KAP_TEST("glob lists directories as well as files")
{
    // Detection rules use dir_exists-style checks (design doc §3.2), so glob
    // must not silently filter out directory entries.
    const std::filesystem::path dir = scratch("glob_dirs");
    std::filesystem::create_directories(dir / "sub.d");
    write_file(dir / "file.d", "x");

    const std::vector<std::string> hits = kap::fs::glob(dir, "*.d");
    KAP_ASSERT_EQ(hits.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(hits[0], "file.d");
    KAP_ASSERT_EQ(hits[1], "sub.d");

    std::filesystem::remove_all(dir);
});

KAP_TEST("glob is not recursive")
{
    // Only entries directly under `dir` are considered; a nested match must
    // not appear, or a plugin's marker-file rule could fire from a vendored
    // subdirectory.
    const std::filesystem::path dir = scratch("glob_flat");
    std::filesystem::create_directories(dir / "nested");
    write_file(dir / "nested" / "Cargo.toml", "x");

    KAP_ASSERT(kap::fs::glob(dir, "Cargo.toml").empty());

    std::filesystem::remove_all(dir);
});

// --- is_within -------------------------------------------------------------------

KAP_TEST("is_within treats the root itself as inside")
{
    const std::filesystem::path dir = scratch("within_self");
    KAP_ASSERT(kap::fs::is_within(dir, dir));
    std::filesystem::remove_all(dir);
});

KAP_TEST("is_within rejects a ../ escape even when it is spelled indirectly")
{
    const std::filesystem::path dir = scratch("within_escape");
    std::filesystem::create_directories(dir / "sub");

    // Textually below the root, but resolves to the parent — the traversal
    // attack design doc §7 exists to stop.
    KAP_ASSERT(!kap::fs::is_within(dir, dir / "sub" / ".." / ".." / "elsewhere"));

    std::filesystem::remove_all(dir);
});

KAP_TEST("is_within is not fooled by a sibling with the root as a name prefix")
{
    // "/tmp/kap_root" and "/tmp/kap_root_evil" share a textual prefix but are
    // different directories; a naive string comparison would accept the second.
    const std::filesystem::path dir     = scratch("prefix");
    const std::filesystem::path sibling = dir.string() + "_evil";
    std::filesystem::create_directories(sibling);

    KAP_ASSERT(!kap::fs::is_within(dir, sibling));

    std::filesystem::remove_all(sibling);
    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text still reads the whole file when the size hint is too small")
{
    // stat is only an allocation hint. This exercises the branch where it
    // under-reports and the buffer has to grow: a file that fills the hinted
    // capacity exactly must not be silently truncated.
    //
    // The hint and the content agree here, so what is really pinned is that
    // the "buffer completely filled" path does not mistake a full buffer for
    // end-of-file. A file whose size is an exact power of two is the shape
    // most likely to expose that off-by-one.
    const std::filesystem::path dir  = scratch("grow_hint");
    const std::filesystem::path file = dir / "big.txt";

    for (const std::size_t size : {std::size_t{1},
                                   std::size_t{2},
                                   std::size_t{4095},
                                   std::size_t{4096},
                                   std::size_t{4097}}) {
        write_file(file, std::string(size, 'z'));
        const std::string got = kap::fs::read_text(file);
        KAP_ASSERT_EQ(got.size(), size);
        KAP_ASSERT_EQ(got, std::string(size, 'z'));
    }

    std::filesystem::remove_all(dir);
});

KAP_TEST("read_text on a file that grows past the cap is still refused")
{
    // The cap must hold even when the stat hint says the file is small. The
    // hint is deliberately made wrong by passing a max_bytes smaller than the
    // real file: capacity comes from stat (larger), so the cap check has to be
    // the thing that catches it.
    const std::filesystem::path dir  = scratch("cap_vs_hint");
    const std::filesystem::path file = dir / "big.txt";
    write_file(file, std::string(5000, 'z'));

    KAP_ASSERT_THROWS(kap::diag::Error, kap::fs::read_text(file, 100));
    KAP_ASSERT_EQ(kap::fs::read_text(file, 5000).size(), static_cast<std::size_t>(5000));

    std::filesystem::remove_all(dir);
});
