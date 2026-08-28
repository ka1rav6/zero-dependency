// tests/test_cli.cpp
//
// Unit tests for the CLI parser (core/cli.hpp). Together with test_toml.cpp
// these are the "parser tests" that Milestone 1's exit criteria demand pass
// 100%.

#include "core/cli.hpp"
#include "core/diag.hpp"
#include "harness.hpp"

#include <cstddef>

KAP_TEST("parses a command with a -- passthrough section")
{
    const kap::cli::Invocation inv = kap::cli::parse({"build", "--", "--release"});
    KAP_ASSERT_EQ(inv.command, "build");
    KAP_ASSERT(inv.argv.empty());
    KAP_ASSERT_EQ(inv.passthrough.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(inv.passthrough[0], "--release");
});

KAP_TEST("global flags may precede or follow the command")
{
    const kap::cli::Invocation a = kap::cli::parse({"--verbose", "build"});
    KAP_ASSERT(a.global.verbose);
    KAP_ASSERT_EQ(a.command, "build");

    const kap::cli::Invocation b = kap::cli::parse({"build", "--verbose"});
    KAP_ASSERT(b.global.verbose);
    KAP_ASSERT_EQ(b.command, "build");
});

KAP_TEST("--dry-run, --root, and -n parse together")
{
    const kap::cli::Invocation inv = kap::cli::parse({"-n", "--root", "/tmp/repo", "test"});
    KAP_ASSERT(inv.global.dry_run);
    KAP_ASSERT(inv.global.root.has_value());
    KAP_ASSERT_EQ(inv.global.root->string(), "/tmp/repo");
    KAP_ASSERT_EQ(inv.command, "test");
});

KAP_TEST("--root=value form works")
{
    const kap::cli::Invocation inv = kap::cli::parse({"--root=/tmp/repo", "build"});
    KAP_ASSERT(inv.global.root.has_value());
    KAP_ASSERT_EQ(inv.global.root->string(), "/tmp/repo");
});

KAP_TEST("--set collects key=value pairs from both forms")
{
    const kap::cli::Invocation inv =
        kap::cli::parse({"--set", "generator=ninja", "build", "--set=build_dir=out"});
    KAP_ASSERT_EQ(inv.global.set_values.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(inv.global.set_values[0], "generator=ninja");
    KAP_ASSERT_EQ(inv.global.set_values[1], "build_dir=out");
});

KAP_TEST("subcommands like plugin install keep their own arguments")
{
    const kap::cli::Invocation inv = kap::cli::parse({"plugin", "install", "cmake-cpp"});
    KAP_ASSERT_EQ(inv.command, "plugin");
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(inv.argv[0], "install");
    KAP_ASSERT_EQ(inv.argv[1], "cmake-cpp");
});

KAP_TEST("config get with a dotted key survives flag parsing")
{
    const kap::cli::Invocation inv = kap::cli::parse({"config", "get", "server.host"});
    KAP_ASSERT_EQ(inv.command, "config");
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(inv.argv[0], "get");
    KAP_ASSERT_EQ(inv.argv[1], "server.host");
});

KAP_TEST("a bare invocation has no command")
{
    const kap::cli::Invocation inv = kap::cli::parse({});
    KAP_ASSERT(inv.command.empty());
    KAP_ASSERT(inv.argv.empty());
    KAP_ASSERT(inv.passthrough.empty());
});

KAP_TEST("unknown options throw a located diagnostic")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::cli::parse({"--bogus", "build"}));
});

KAP_TEST("--set without an '=' in the value is rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::cli::parse({"--set", "nope", "build"}));
});

KAP_TEST("--root at the end of the line is rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::cli::parse({"build", "--root"}));
});