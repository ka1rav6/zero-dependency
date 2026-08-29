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

// --- Passthrough (`--`) ----------------------------------------------------------

KAP_TEST("everything after -- is passthrough, including things that look like flags")
{
    // The point of `--`: kap must not try to interpret the wrapped tool's
    // flags. `--verbose` here belongs to the tool, not to kap.
    const kap::cli::Invocation inv =
        kap::cli::parse({"build", "--", "--verbose", "--root", "-n", "--not-a-kap-flag"});

    KAP_ASSERT_EQ(inv.command, "build");
    KAP_ASSERT(inv.argv.empty());
    KAP_ASSERT_EQ(inv.passthrough.size(), static_cast<std::size_t>(4));
    KAP_ASSERT_EQ(inv.passthrough[0], "--verbose");
    KAP_ASSERT_EQ(inv.passthrough[2], "-n");

    // Those flags must NOT have been applied to kap itself.
    KAP_ASSERT(!inv.global.verbose);
    KAP_ASSERT(!inv.global.dry_run);
    KAP_ASSERT(!inv.global.root.has_value());
});

KAP_TEST("a second -- is passed through literally")
{
    // Only the first `--` is a separator; later ones are the tool's business.
    const kap::cli::Invocation inv = kap::cli::parse({"run", "--", "a", "--", "b"});

    KAP_ASSERT_EQ(inv.passthrough.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(inv.passthrough[1], "--");
});

KAP_TEST("a trailing -- yields an empty passthrough, not an error")
{
    const kap::cli::Invocation inv = kap::cli::parse({"build", "--"});
    KAP_ASSERT_EQ(inv.command, "build");
    KAP_ASSERT(inv.passthrough.empty());
});

KAP_TEST("passthrough words are preserved verbatim, including empty ones")
{
    const kap::cli::Invocation inv = kap::cli::parse({"run", "--", "", "a b", "x=y"});

    KAP_ASSERT_EQ(inv.passthrough.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(inv.passthrough[0], "");
    KAP_ASSERT_EQ(inv.passthrough[1], "a b");
});

// --- Positional handling ---------------------------------------------------------

KAP_TEST("a lone dash is a positional, not an unknown flag")
{
    // "-" conventionally means stdin; it must survive as an argument rather
    // than being rejected as a malformed option.
    const kap::cli::Invocation inv = kap::cli::parse({"run", "-"});

    KAP_ASSERT_EQ(inv.command, "run");
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(inv.argv[0], "-");
});

KAP_TEST("the first positional is the command and the rest keep their order")
{
    const kap::cli::Invocation inv =
        kap::cli::parse({"plugin", "install", "cmake-cpp", "--verbose"});

    KAP_ASSERT_EQ(inv.command, "plugin");
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(inv.argv[0], "install");
    KAP_ASSERT_EQ(inv.argv[1], "cmake-cpp");
    KAP_ASSERT(inv.global.verbose);
});

KAP_TEST("an empty argument list parses to an empty invocation")
{
    const kap::cli::Invocation inv = kap::cli::parse({});

    KAP_ASSERT(inv.command.empty());
    KAP_ASSERT(inv.argv.empty());
    KAP_ASSERT(inv.passthrough.empty());
    KAP_ASSERT(!inv.global.help);
    KAP_ASSERT(!inv.global.version);
});

// --- Flag details ----------------------------------------------------------------

KAP_TEST("repeating a boolean flag is harmless")
{
    const kap::cli::Invocation inv = kap::cli::parse({"-n", "--dry-run", "build", "-n"});
    KAP_ASSERT(inv.global.dry_run);
    KAP_ASSERT_EQ(inv.command, "build");
});

KAP_TEST("the last --root wins")
{
    const kap::cli::Invocation inv =
        kap::cli::parse({"--root", "/first", "--root=/second", "build"});
    KAP_ASSERT(inv.global.root.has_value());
    KAP_ASSERT_EQ(inv.global.root->string(), "/second");
});

KAP_TEST("--set keeps every pair, in order, and splits only on the first =")
{
    const kap::cli::Invocation inv =
        kap::cli::parse({"--set", "a=1", "--set=b=2", "--set", "c=x=y", "build"});

    KAP_ASSERT_EQ(inv.global.set_values.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(inv.global.set_values[0], "a=1");
    KAP_ASSERT_EQ(inv.global.set_values[1], "b=2");
    // The value may itself contain '='; only the first one is the separator.
    KAP_ASSERT_EQ(inv.global.set_values[2], "c=x=y");
});

KAP_TEST("--set accepts an empty value")
{
    // "key=" clears a setting; it has an '=' so it is well-formed.
    const kap::cli::Invocation inv = kap::cli::parse({"--set", "key=", "build"});
    KAP_ASSERT_EQ(inv.global.set_values[0], "key=");
});

KAP_TEST("--set= with nothing after it is rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::cli::parse({"--set=", "build"}));
});

KAP_TEST("--root= with nothing after it is rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::cli::parse({"--root=", "build"}));
});

KAP_TEST("--set at the end of the line is rejected")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::cli::parse({"build", "--set"}));
});

KAP_TEST("a --root value that looks like a flag is still taken as the path")
{
    // `--root --verbose` is a user mistake, but consuming the next token is
    // the documented behaviour and is unambiguous; it must not silently set
    // --verbose instead.
    const kap::cli::Invocation inv = kap::cli::parse({"--root", "--verbose", "build"});
    KAP_ASSERT_EQ(inv.global.root->string(), "--verbose");
    KAP_ASSERT(!inv.global.verbose);
});

KAP_TEST("parse failures name the offending option")
{
    try {
        (void) kap::cli::parse({"--frobnicate"});
        KAP_ASSERT(false); // unreachable
    }
    catch (const kap::diag::Error& e) {
        KAP_ASSERT(e.report().find("--frobnicate") != std::string::npos);
        // CLI errors have no source file, so they carry the synthetic <argv>.
        KAP_ASSERT_EQ(e.diagnostic().location.file, "<argv>");
    }
});

KAP_TEST("--help before a command asks for kap's own usage")
{
    const kap::cli::Invocation inv = kap::cli::parse({"--help"});
    KAP_ASSERT(inv.global.help);
    KAP_ASSERT(inv.command.empty());

    const kap::cli::Invocation v = kap::cli::parse({"-V"});
    KAP_ASSERT(v.global.version);
});

KAP_TEST("--help after a command asks about that command")
{
    // `kap install -h` used to set the *global* help flag and print kap's
    // banner — the one place someone is already confused, answered with a page
    // that says nothing about what they asked. It is now forwarded like any
    // other command-local option, and the subcommand prints its own page.
    for (const char* flag : {"-h", "--help"}) {
        const kap::cli::Invocation inv = kap::cli::parse({"install", flag});
        KAP_ASSERT(!inv.global.help);
        KAP_ASSERT_EQ(inv.command, std::string("install"));
        KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(1));
        KAP_ASSERT_EQ(inv.argv.front(), std::string(flag));
    }
});

KAP_TEST("--help reaches a subcommand's own arguments")
{
    // `plugin` is the command; `install --help` is what it receives, so the
    // plugin dispatcher can print the page for that specific subcommand.
    const kap::cli::Invocation inv = kap::cli::parse({"plugin", "install", "--help"});
    KAP_ASSERT(!inv.global.help);
    KAP_ASSERT_EQ(inv.command, std::string("plugin"));
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(inv.argv[0], std::string("install"));
    KAP_ASSERT_EQ(inv.argv[1], std::string("--help"));
});

KAP_TEST("--help after `--` is passthrough, not a request for help")
{
    // `kap test -- --help` asks the *test runner* for its help.
    const kap::cli::Invocation inv = kap::cli::parse({"test", "--", "--help"});
    KAP_ASSERT(!inv.global.help);
    KAP_ASSERT(inv.argv.empty());
    KAP_ASSERT_EQ(inv.passthrough.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(inv.passthrough.front(), std::string("--help"));
});

// --- command-local options (Milestone 4) ------------------------------------------
//
// `kap detect --refresh` and `kap plugin install --link ./x` are options that
// belong to a subcommand, not to kap. The global parser cannot validate them
// without duplicating every subcommand's option table, so it forwards them.

KAP_TEST("an option after the command word is forwarded to the command")
{
    const kap::cli::Invocation inv = kap::cli::parse({"detect", "--refresh"});
    KAP_ASSERT_EQ(inv.command, std::string("detect"));
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(inv.argv.front(), std::string("--refresh"));
});

KAP_TEST("an unknown option before the command word is still refused")
{
    // Nothing has been named that it could belong to, so guessing would be
    // strictly worse than failing.
    KAP_ASSERT_THROWS(kap::diag::Error, kap::cli::parse({"--bogus", "detect"}));
});

KAP_TEST("global flags still win over command forwarding, wherever they appear")
{
    const kap::cli::Invocation inv =
        kap::cli::parse({"plugin", "install", "--link", "./x", "--verbose", "-n"});
    KAP_ASSERT(inv.global.verbose);
    KAP_ASSERT(inv.global.dry_run);
    KAP_ASSERT_EQ(inv.command, std::string("plugin"));
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(inv.argv[0], std::string("install"));
    KAP_ASSERT_EQ(inv.argv[1], std::string("--link"));
    KAP_ASSERT_EQ(inv.argv[2], std::string("./x"));
});

KAP_TEST("forwarding preserves argument order, which subcommands rely on")
{
    const kap::cli::Invocation inv = kap::cli::parse({"plugin", "test", "-v", "name", "--json"});
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(4));
    KAP_ASSERT_EQ(inv.argv[0], std::string("test"));
    KAP_ASSERT_EQ(inv.argv[1], std::string("-v"));
    KAP_ASSERT_EQ(inv.argv[2], std::string("name"));
    KAP_ASSERT_EQ(inv.argv[3], std::string("--json"));
});

KAP_TEST("a command-local option never leaks into passthrough")
{
    const kap::cli::Invocation inv = kap::cli::parse({"build", "--quiet", "--", "--release"});
    KAP_ASSERT_EQ(inv.argv.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(inv.argv.front(), std::string("--quiet"));
    KAP_ASSERT_EQ(inv.passthrough.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(inv.passthrough.front(), std::string("--release"));
});
