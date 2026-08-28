// tests/test_argv.cpp
//
// Unit tests for the argv display/escaping helpers (core/argv.hpp). The
// quoting rules here are load-bearing for `--dry-run` output: the tests pin
// exactly what a dry run will print for tricky words.

#include "core/argv.hpp"
#include "harness.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

KAP_TEST("an empty list knows it is empty")
{
    const kap::argv::List l;
    KAP_ASSERT(l.empty());
    KAP_ASSERT_EQ(l.size(), static_cast<std::size_t>(0));
});

KAP_TEST("initializer-list construction preserves order")
{
    const kap::argv::List l = {"cmake", "-S", ".", "-B", "build"};
    KAP_ASSERT_EQ(l.size(), static_cast<std::size_t>(5));

    // Spot-check every position: order is the whole contract of an argv array,
    // and checking only one index is how an off-by-one slips through.
    KAP_ASSERT_EQ(l[0], "cmake");
    KAP_ASSERT_EQ(l[1], "-S");
    KAP_ASSERT_EQ(l[2], ".");
    KAP_ASSERT_EQ(l[3], "-B");
    KAP_ASSERT_EQ(l[4], "build");
});

KAP_TEST("operator[] bounds-checks instead of reading out of range")
{
    // List::operator[] forwards to std::vector::at, so an out-of-range index
    // throws rather than silently returning garbage.
    const kap::argv::List l = {"cmake"};
    bool threw = false;
    try {
        (void)l[5];
    } catch (const std::out_of_range&) {
        threw = true;
    }
    KAP_ASSERT(threw);
});

KAP_TEST("vec() exposes the underlying storage unchanged")
{
    const kap::argv::List l = {"a", "b"};
    KAP_ASSERT_EQ(l.vec().size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(l.vec()[1], "b");
});

KAP_TEST("push_back grows the list one word at a time")
{
    kap::argv::List l;
    l.push_back("cmake");
    l.push_back("--version");
    KAP_ASSERT(!l.empty());
    KAP_ASSERT_EQ(l.joined(), "cmake --version");
});

KAP_TEST("explicit vector construction takes ownership of the words")
{
    const kap::argv::List l{std::vector<std::string>{"go", "test", "./..."}};
    KAP_ASSERT_EQ(l.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(l.quoted(), "go test ./...");
});

KAP_TEST("joined and quoted are empty for an empty list")
{
    const kap::argv::List l;
    KAP_ASSERT_EQ(l.joined(), "");
    KAP_ASSERT_EQ(l.quoted(), "");
});

KAP_TEST("joined concatenates with the separator")
{
    const kap::argv::List l = {"a", "b", "c"};
    KAP_ASSERT_EQ(l.joined(), "a b c");
    KAP_ASSERT_EQ(l.joined(", "), "a, b, c");
});

KAP_TEST("quoted leaves safe words untouched for readable dry-runs")
{
    const kap::argv::List l = {"cmake", "--build", "build"};
    KAP_ASSERT_EQ(l.quoted(), "cmake --build build");
});

KAP_TEST("quoted wraps words containing spaces")
{
    const kap::argv::List l = {"npm", "run", "my dev server"};
    KAP_ASSERT_EQ(l.quoted(), "npm run 'my dev server'");
});

KAP_TEST("escape_word quotes embedded single quotes per POSIX")
{
    KAP_ASSERT_EQ(kap::argv::escape_word("it's"), "'it'\\''s'");
});

KAP_TEST("escape_word quotes an empty word")
{
    KAP_ASSERT_EQ(kap::argv::escape_word(""), "''");
});

KAP_TEST("append extends a list in place")
{
    kap::argv::List l = {"cmake"};
    l.append({"--build", "build"});
    l.append(std::vector<std::string>{"-j", "4"});

    KAP_ASSERT_EQ(l.size(), static_cast<std::size_t>(5));
    KAP_ASSERT_EQ(l.quoted(), "cmake --build build -j 4");
});