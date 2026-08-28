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
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("kap_test_" + name + "_" + std::to_string(getpid()));
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
    KAP_ASSERT_THROWS(kap::diag::Error, kap::fs::read_text(std::filesystem::temp_directory_path() / "kap_no_such_file_xyz"));
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
    const std::filesystem::path dir = scratch("within");
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