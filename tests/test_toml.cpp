// tests/test_toml.cpp
//
// Unit tests for the minimal TOML subset parser (core/toml.hpp). Milestone 1
// requires 100% of parser tests to pass, so this file is the contract: the
// subset that works today, and the malformed inputs that must fail loudly.

#include "core/diag.hpp"
#include "core/toml.hpp"
#include "harness.hpp"

#include <cstdint>
#include <string>

KAP_TEST("parses top-level scalars")
{
    const kap::toml::Document doc = kap::toml::parse("name = \"kap\"\ncount = 42\nactive = true\n");

    const auto name = doc.get("name");
    const auto count = doc.get("count");
    const auto active = doc.get("active");

    KAP_ASSERT(name.has_value());
    KAP_ASSERT(name->kind == kap::toml::Value::Kind::String);
    KAP_ASSERT_EQ(name->str, "kap");

    KAP_ASSERT(count.has_value());
    KAP_ASSERT(count->kind == kap::toml::Value::Kind::Integer);
    KAP_ASSERT_EQ(count->integer, static_cast<std::int64_t>(42));

    KAP_ASSERT(active.has_value());
    KAP_ASSERT(active->kind == kap::toml::Value::Kind::Boolean);
    KAP_ASSERT(active->boolean);
});

KAP_TEST("negative integers parse")
{
    const kap::toml::Document doc = kap::toml::parse("delta = -7\n");
    KAP_ASSERT_EQ(doc.get("delta")->integer, static_cast<std::int64_t>(-7));
});

KAP_TEST("parses nested tables and dotted lookups")
{
    const kap::toml::Document doc = kap::toml::parse(
        "[plugins.cmake-cpp]\ngenerator = \"ninja\"\nbuild_dir = \"out\"\n");

    const auto gen = doc.get("plugins.cmake-cpp.generator");
    KAP_ASSERT(gen.has_value());
    KAP_ASSERT_EQ(gen->str, "ninja");

    // A missing leaf is nullopt, not an error.
    KAP_ASSERT(!doc.get("plugins.cmake-cpp.missing").has_value());
    // A path that steps through a scalar is nullopt.
    KAP_ASSERT(!doc.get("plugins.no-such-table.deep").has_value());
});

KAP_TEST("parses arrays of strings and integers")
{
    const kap::toml::Document doc =
        kap::toml::parse("cmake_args = [\"-DCMAKE_BUILD_TYPE=Debug\", \"-j\"]\nports = [80, 443]\n");

    const auto args = doc.get("cmake_args");
    KAP_ASSERT(args.has_value());
    KAP_ASSERT(args->kind == kap::toml::Value::Kind::Array);
    KAP_ASSERT_EQ(args->array.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(args->array[0].str, "-DCMAKE_BUILD_TYPE=Debug");
    KAP_ASSERT_EQ(args->array[1].str, "-j");

    const auto ports = doc.get("ports");
    KAP_ASSERT_EQ(ports->array[1].integer, static_cast<std::int64_t>(443));
});

KAP_TEST("empty arrays parse")
{
    const kap::toml::Document doc = kap::toml::parse("xs = []\n");
    const auto xs = doc.get("xs");
    KAP_ASSERT(xs.has_value());
    KAP_ASSERT(xs->kind == kap::toml::Value::Kind::Array);
    KAP_ASSERT(xs->array.empty());
});

KAP_TEST("string escape sequences decode")
{
    const kap::toml::Document doc = kap::toml::parse("s = \"a\\nb\\t\\\"c\\\\\"\n");
    KAP_ASSERT_EQ(doc.get("s")->str, std::string("a\nb\t\"c\\"));
});

KAP_TEST("comments are stripped, including trailing ones")
{
    const kap::toml::Document doc = kap::toml::parse("# header comment\na = 1 # trailing\n\n# another\n");
    KAP_ASSERT_EQ(doc.get("a")->integer, static_cast<std::int64_t>(1));
});

KAP_TEST("parse errors carry a located diagnostic (line:col)")
{
    const std::string text = "a = 1\nb = @\n";
    try {
        kap::toml::parse(text, "kap.toml");
        KAP_ASSERT(false);   // unreachable: this input must fail
    } catch (const kap::diag::Error& e) {
        KAP_ASSERT(e.diagnostic().location.line == 2);
        KAP_ASSERT(e.diagnostic().location.col >= 1);
        KAP_ASSERT(e.report().find("kap.toml:2:") != std::string::npos);
    }
});

KAP_TEST("duplicate keys are rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("a = 1\na = 2\n"));
});

KAP_TEST("unknown escape sequences are rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("s = \"\\q\"\n"));
});

KAP_TEST("unterminated strings are rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("s = \"unclosed\n"));
});

KAP_TEST("table conflicts with an existing scalar are rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("plugins = \"gone\"\n[plugins.cmake-cpp]\n"));
});
// --- Regression tests for the section-header retargeting bug ---------------------
// A `[header]` line must re-point where subsequent `key = value` lines land.
// The original parser created the table but kept writing keys into the document
// root, so `[server]` + `host = ...` produced a top-level `host`. These tests
// pin the behaviour from both directions: the key IS under the section, and it
// is NOT at the root.

KAP_TEST("keys after a table header land inside that table, not at the root")
{
    const kap::toml::Document doc = kap::toml::parse("[server]\nhost = \"localhost\"\nport = 8080\n");

    KAP_ASSERT(doc.get("server.host").has_value());
    KAP_ASSERT_EQ(doc.get("server.host")->str, "localhost");
    KAP_ASSERT_EQ(doc.get("server.port")->integer, static_cast<std::int64_t>(8080));

    // The bug's signature: the key must NOT have leaked to the document root.
    KAP_ASSERT(!doc.get("host").has_value());
    KAP_ASSERT(!doc.get("port").has_value());
});

KAP_TEST("consecutive table headers each capture their own keys")
{
    const kap::toml::Document doc =
        kap::toml::parse("[a]\nx = 1\n\n[b]\nx = 2\n\n[c.d]\nx = 3\n");

    KAP_ASSERT_EQ(doc.get("a.x")->integer, static_cast<std::int64_t>(1));
    KAP_ASSERT_EQ(doc.get("b.x")->integer, static_cast<std::int64_t>(2));
    KAP_ASSERT_EQ(doc.get("c.d.x")->integer, static_cast<std::int64_t>(3));
    KAP_ASSERT(!doc.get("x").has_value());
});

KAP_TEST("keys before the first header stay at the document root")
{
    const kap::toml::Document doc = kap::toml::parse("version = 2\n[server]\nport = 80\n");

    KAP_ASSERT_EQ(doc.get("version")->integer, static_cast<std::int64_t>(2));
    KAP_ASSERT_EQ(doc.get("server.port")->integer, static_cast<std::int64_t>(80));
});

KAP_TEST("a table header is always resolved from the root, never nested")
{
    // `[b]` must be a sibling of `[a]`, not `a.b` — headers are absolute.
    const kap::toml::Document doc = kap::toml::parse("[a]\nx = 1\n[b]\ny = 2\n");

    KAP_ASSERT(doc.get("b.y").has_value());
    KAP_ASSERT(!doc.get("a.b.y").has_value());
});

KAP_TEST("the same key name may repeat in different tables")
{
    // This only works if each header really retargets: otherwise both `name`
    // lines would hit the root table and trip the duplicate-key check.
    const kap::toml::Document doc =
        kap::toml::parse("[one]\nname = \"first\"\n[two]\nname = \"second\"\n");

    KAP_ASSERT_EQ(doc.get("one.name")->str, "first");
    KAP_ASSERT_EQ(doc.get("two.name")->str, "second");
});

KAP_TEST("a table defined twice is rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("[server]\na = 1\n[server]\nb = 2\n"));
});

// --- Lexical robustness ----------------------------------------------------------

KAP_TEST("CRLF line endings parse the same as LF")
{
    const kap::toml::Document doc =
        kap::toml::parse("# comment\r\n[server]\r\nhost = \"localhost\"\r\nport = 8080\r\n");

    KAP_ASSERT_EQ(doc.get("server.host")->str, "localhost");
    KAP_ASSERT_EQ(doc.get("server.port")->integer, static_cast<std::int64_t>(8080));
});

KAP_TEST("arrays may span multiple lines and carry a trailing comma")
{
    const kap::toml::Document doc = kap::toml::parse(
        "args = [\n  \"-DCMAKE_BUILD_TYPE=Debug\",   # why not\n  \"-Wall\",\n]\n");

    const auto args = doc.get("args");
    KAP_ASSERT(args.has_value());
    KAP_ASSERT_EQ(args->array.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(args->array[1].str, "-Wall");
});

KAP_TEST("spaces are allowed around the dots of a table header")
{
    const kap::toml::Document doc = kap::toml::parse("[ plugins . cmake-cpp ]\ngenerator = \"ninja\"\n");
    KAP_ASSERT_EQ(doc.get("plugins.cmake-cpp.generator")->str, "ninja");
});

KAP_TEST("integers with leading zeros are rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("port = 0080\n"));
});

KAP_TEST("a bare zero is still a valid integer")
{
    KAP_ASSERT_EQ(kap::toml::parse("n = 0\n").get("n")->integer, static_cast<std::int64_t>(0));
});

KAP_TEST("an explicit plus sign parses")
{
    KAP_ASSERT_EQ(kap::toml::parse("n = +5\n").get("n")->integer, static_cast<std::int64_t>(5));
});

KAP_TEST("integers that overflow int64 are rejected, not wrapped")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("n = 99999999999999999999\n"));
});

KAP_TEST("a string ending in a lone backslash is unterminated, not a crash")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("s = \"oops\\"));
});

KAP_TEST("an unclosed array is rejected rather than looping forever")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::toml::parse("xs = [1, 2\n"));
});

KAP_TEST("Value factories set the kind and payload together")
{
    KAP_ASSERT(kap::toml::is_string(kap::toml::make_string("x")));
    KAP_ASSERT_EQ(kap::toml::make_string("x").str, "x");
    KAP_ASSERT_EQ(kap::toml::make_integer(-3).integer, static_cast<std::int64_t>(-3));
    KAP_ASSERT(kap::toml::make_boolean(true).boolean);
    KAP_ASSERT(kap::toml::is_array(kap::toml::make_array()));
    KAP_ASSERT(kap::toml::is_table(kap::toml::make_table()));
});
