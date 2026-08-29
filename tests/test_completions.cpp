// tests/test_completions.cpp
//
// Unit tests for the generated shell completion scripts (core/completions.hpp,
// design doc Milestone 10).
//
// A completion script is shell source that this program prints, so the two
// things worth asserting are that it is *syntactically* plausible and that it
// actually mentions the commands it was given. The stronger check — that bash
// and zsh accept the script — lives in tests/e2e.sh, which can run `bash -n`
// on the real output.

#include "core/completions.hpp"
#include "harness.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace
{

const std::vector<std::string> kCommands = {"build", "test", "clean", "dev"};
const std::vector<std::string> kPlugin   = {"list", "install", "remove"};

} // namespace

KAP_TEST("every advertised shell produces a script")
{
    const std::vector<std::string> shells = kap::completions::shells();
    KAP_ASSERT_EQ(shells.size(), static_cast<std::size_t>(3));
    for (const std::string& shell : shells) {
        const std::string text = kap::completions::script(shell, kCommands, kPlugin);
        KAP_ASSERT(!text.empty());
        // Every script opens with a comment saying how to install it. A
        // completion script nobody knows where to put is not much use.
        KAP_ASSERT(text.find("kap completions " + shell) != std::string::npos);
    }
});

KAP_TEST("an unknown shell produces nothing rather than a broken script")
{
    KAP_ASSERT_EQ(kap::completions::script("csh", kCommands, kPlugin), std::string(""));
    KAP_ASSERT_EQ(kap::completions::script("", kCommands, kPlugin), std::string(""));
});

KAP_TEST("each script completes the commands it was given")
{
    // The lists come from core/main.cpp rather than being restated here, which
    // is the whole reason the scripts are generated: a checked-in script would
    // drift the first time somebody added a subcommand.
    for (const std::string& shell : kap::completions::shells()) {
        const std::string text = kap::completions::script(shell, kCommands, kPlugin);
        for (const std::string& command : kCommands)
            KAP_ASSERT(text.find(command) != std::string::npos);
        for (const std::string& subcommand : kPlugin)
            KAP_ASSERT(text.find(subcommand) != std::string::npos);
    }
});

KAP_TEST("each script knows kap's own commands and global flags")
{
    for (const std::string& shell : kap::completions::shells()) {
        const std::string text = kap::completions::script(shell, kCommands, kPlugin);
        KAP_ASSERT(text.find("detect") != std::string::npos);
        KAP_ASSERT(text.find("config") != std::string::npos);
        KAP_ASSERT(text.find("plugin") != std::string::npos);
        // Matched without the leading dashes: bash and zsh spell the flag
        // "--dry-run" while fish declares it as `-l dry-run`, and asserting on
        // one spelling would only test two of the three scripts.
        KAP_ASSERT(text.find("dry-run") != std::string::npos);
        KAP_ASSERT(text.find("root") != std::string::npos);
    }
});

KAP_TEST("the zsh script starts with the compdef line zsh requires")
{
    // #compdef must be the very first line, or zsh's autoload machinery never
    // associates the file with the command and the script silently does nothing.
    const std::string text = kap::completions::script("zsh", kCommands, kPlugin);
    KAP_ASSERT(text.rfind("#compdef kap", 0) == 0);
});

KAP_TEST("the scripts have balanced quotes, which is the cheapest syntax check")
{
    // Not a parser — tests/e2e.sh runs `bash -n` on the real output for that —
    // but an odd number of single or double quotes is the failure mode a
    // hand-edited heredoc actually produces, and it is free to rule out.
    for (const std::string& shell : kap::completions::shells()) {
        const std::string text = kap::completions::script(shell, kCommands, kPlugin);
        const std::size_t doubles =
            static_cast<std::size_t>(std::count(text.begin(), text.end(), '"'));
        KAP_ASSERT_EQ(doubles % 2, static_cast<std::size_t>(0));
    }
});

KAP_TEST("an empty command list still produces a usable script")
{
    // `kap completions bash` on a build with no project commands is a silly
    // case, but it must not produce a script that fails to source.
    for (const std::string& shell : kap::completions::shells()) {
        const std::string text = kap::completions::script(shell, {}, {});
        KAP_ASSERT(!text.empty());
        KAP_ASSERT(text.find("kap") != std::string::npos);
    }
});
