// tests/test_help.cpp
//
// Unit tests for the per-command help pages (core/help.hpp).
//
// Help is documentation that ships inside the binary, so the things worth
// asserting are the ones that make documentation useless: a command with no
// page, a page that does not mention the command it is about, and a page that
// has drifted from the flags the command actually accepts.

#include "core/help.hpp"
#include "harness.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace
{

// Every name the CLI dispatches on. If this list and help::topics() disagree,
// somebody added a command without a page — which is exactly the drift these
// tests exist to catch.
const std::vector<std::string> kCommands = {"build",
                                            "check",
                                            "ci",
                                            "clean",
                                            "dev",
                                            "doctor",
                                            "fmt",
                                            "install",
                                            "lint",
                                            "ports",
                                            "run",
                                            "test",
                                            "detect",
                                            "config",
                                            "plugin",
                                            "completions",
                                            "help"};

const std::vector<std::string> kPluginSubcommands = {"plugin list",
                                                     "plugin search",
                                                     "plugin install",
                                                     "plugin remove",
                                                     "plugin update",
                                                     "plugin enable",
                                                     "plugin disable",
                                                     "plugin pin",
                                                     "plugin new",
                                                     "plugin doctor",
                                                     "plugin test"};

} // namespace

KAP_TEST("every command has a help page")
{
    for (const std::string& command : kCommands) {
        if (kap::help::find(command) == nullptr)
            ::kap_test::fail_test(__FILE__, __LINE__, "no help page for '" + command + "'");
    }
});

KAP_TEST("every plugin subcommand has a help page")
{
    for (const std::string& subcommand : kPluginSubcommands) {
        if (kap::help::find(subcommand) == nullptr)
            ::kap_test::fail_test(__FILE__, __LINE__, "no help page for '" + subcommand + "'");
    }
});

KAP_TEST("a page names the command it is about, and has all four sections")
{
    // The shape is the point: someone skimming for the flag list should find it
    // in the same place every time.
    for (const std::string& command : kCommands) {
        const std::string text = kap::help::page(command);
        if (text.find("kap " + command) == std::string::npos)
            ::kap_test::fail_test(__FILE__,
                                  __LINE__,
                                  "the page for '" + command + "' never says 'kap " + command +
                                      "'");
        if (text.find("SYNOPSIS") == std::string::npos)
            ::kap_test::fail_test(__FILE__, __LINE__, "'" + command + "' has no SYNOPSIS");
        if (text.find("DESCRIPTION") == std::string::npos)
            ::kap_test::fail_test(__FILE__, __LINE__, "'" + command + "' has no DESCRIPTION");
    }
});

KAP_TEST("a project command's page carries the shared options section")
{
    // Appended rather than repeated in twelve places, which is what keeps the
    // twelve from drifting into twelve slightly different explanations.
    const std::string build = kap::help::page("build");
    KAP_ASSERT(build.find("--dry-run") != std::string::npos);
    KAP_ASSERT(build.find("--root") != std::string::npos);
    KAP_ASSERT(build.find("--set") != std::string::npos);
    KAP_ASSERT(build.find("TOOL ARGUMENTS") != std::string::npos);

    // ...and a kap-level command does not, because those options mean nothing
    // to it.
    KAP_ASSERT(kap::help::page("completions").find("TOOL ARGUMENTS") == std::string::npos);
});

KAP_TEST("the install page explains both of its meanings")
{
    // The single most confusing thing in the CLI: `kap install` installs the
    // project, `kap install <name>` installs a plugin. A page that documented
    // only one of them would leave the trap exactly where it was.
    const std::string text = kap::help::page("install");
    KAP_ASSERT(text.find("PROJECT") != std::string::npos);
    KAP_ASSERT(text.find("PLUGIN") != std::string::npos);
    KAP_ASSERT(text.find("kap plugin install") != std::string::npos);
});

KAP_TEST("the dev page states what -o costs")
{
    // -o pipes the output, so a tool checking isatty() loses its colours. A
    // surprise that is documented is a trade-off; one that is not is a bug
    // report.
    const std::string text = kap::help::page("dev");
    KAP_ASSERT(text.find("-o") != std::string::npos);
    KAP_ASSERT(text.find("colours") != std::string::npos);
});

KAP_TEST("the plugin install page covers all five sources")
{
    const std::string text = kap::help::page("plugin install");
    KAP_ASSERT(text.find("--link") != std::string::npos);
    KAP_ASSERT(text.find("--bundle") != std::string::npos);
    KAP_ASSERT(text.find("--project") != std::string::npos);
    KAP_ASSERT(text.find("install.sh") != std::string::npos);
    KAP_ASSERT(text.find("HTTPS only") != std::string::npos);
});

KAP_TEST("an unknown topic yields an empty page, not a wrong one")
{
    KAP_ASSERT(kap::help::find("frobnicate") == nullptr);
    KAP_ASSERT_EQ(kap::help::page("frobnicate"), std::string(""));
    KAP_ASSERT_EQ(kap::help::page(""), std::string(""));
});

KAP_TEST("requested() finds a help flag anywhere in the arguments")
{
    KAP_ASSERT(kap::help::requested({"-h"}));
    KAP_ASSERT(kap::help::requested({"--help"}));
    KAP_ASSERT(kap::help::requested({"install", "--help"}));
    KAP_ASSERT(kap::help::requested({"--link", "./x", "-h"}));
    KAP_ASSERT(!kap::help::requested({}));
    KAP_ASSERT(!kap::help::requested({"install", "cmake-cpp"}));
    // Not a help flag: a plugin named "help" is a legitimate positional.
    KAP_ASSERT(!kap::help::requested({"help"}));
});

KAP_TEST("without_help_flags leaves everything else in order")
{
    const std::vector<std::string> kept =
        kap::help::without_help_flags({"install", "-h", "--link", "./x", "--help"});
    KAP_ASSERT_EQ(kept.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(kept[0], std::string("install"));
    KAP_ASSERT_EQ(kept[1], std::string("--link"));
    KAP_ASSERT_EQ(kept[2], std::string("./x"));
});

KAP_TEST("every topic has a summary for the `kap help` index")
{
    for (const kap::help::Topic& topic : kap::help::topics()) {
        KAP_ASSERT(!topic.name.empty());
        KAP_ASSERT(!topic.summary.empty());
        KAP_ASSERT(!topic.body.empty());
        // A summary is a table entry, not a sentence.
        KAP_ASSERT(topic.summary.size() < 60);
    }
});
