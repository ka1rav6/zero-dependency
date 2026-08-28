// tests/test_kpl.cpp
//
// Lexer contract for the KPL front-end (design doc Milestone 2).

#include "core/diag.hpp"
#include "core/kpl.hpp"
#include "harness.hpp"

#include <cstddef>

KAP_TEST("KPL lexer handles comments, literals, and operators")
{
    const auto tokens = kap::kpl::lex("// header\nmanifest { name = \"cmake-cpp\" priority = -2 }\n"
                                      "if a && b != 3 { step [\"cmake\"] + extra }\n");

    KAP_ASSERT_EQ(tokens[0].text, "manifest");
    KAP_ASSERT_EQ(static_cast<int>(tokens[1].kind),
                  static_cast<int>(kap::kpl::TokenKind::LeftBrace));
    KAP_ASSERT_EQ(static_cast<int>(tokens[3].kind), static_cast<int>(kap::kpl::TokenKind::Equal));
    KAP_ASSERT_EQ(tokens[4].text, "cmake-cpp");
    KAP_ASSERT_EQ(static_cast<int>(tokens[7].kind), static_cast<int>(kap::kpl::TokenKind::Minus));
    KAP_ASSERT_EQ(tokens[8].integer, static_cast<std::int64_t>(2));
    KAP_ASSERT_EQ(static_cast<int>(tokens[12].kind), static_cast<int>(kap::kpl::TokenKind::AndAnd));
    KAP_ASSERT_EQ(static_cast<int>(tokens[14].kind),
                  static_cast<int>(kap::kpl::TokenKind::BangEqual));
    KAP_ASSERT_EQ(static_cast<int>(tokens.back().kind), static_cast<int>(kap::kpl::TokenKind::End));
});

KAP_TEST("KPL lexer decodes supported string escapes")
{
    const auto tokens = kap::kpl::lex("x = \"line\\n\\t\\\"quote\\\\\"\n");
    KAP_ASSERT_EQ(tokens[2].text, std::string("line\n\t\"quote\\"));
});

KAP_TEST("KPL lexer tracks line and column")
{
    const auto tokens = kap::kpl::lex("x = 1\n  y = true\n", "plugin.kpl");
    KAP_ASSERT_EQ(tokens[3].line, 2);
    KAP_ASSERT_EQ(tokens[3].column, 3);
});

KAP_TEST("KPL lexer rejects malformed input with a location")
{
    try {
        kap::kpl::lex("manifest { name = @ }\n", "plugin.kpl");
        KAP_ASSERT(false);
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT_EQ(error.diagnostic().location.line, 1);
        KAP_ASSERT_EQ(error.diagnostic().location.col, 19);
        KAP_ASSERT(error.report().find("plugin.kpl:1:19") != std::string::npos);
    }
});

KAP_TEST("KPL parser builds plugin blocks and command statements")
{
    const auto plugin = kap::kpl::parse("manifest { name = \"demo\" priority = 10 }\n"
                                        "detect { file_exists \"CMakeLists.txt\" }\n"
                                        "command build(project, config, extra) {\n"
                                        "  let args = [\"cmake\"] + extra\n"
                                        "  if config.release { step args } else { step \"make\" }\n"
                                        "}\n",
                                        "plugin.kpl");

    KAP_ASSERT(plugin.manifest.has_value());
    KAP_ASSERT(plugin.detect.has_value());
    KAP_ASSERT_EQ(plugin.commands.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(plugin.commands[0].name, "build");
    KAP_ASSERT_EQ(plugin.commands[0].parameters.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(plugin.commands[0].body.statements.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(static_cast<int>(plugin.commands[0].body.statements[0].kind),
                  static_cast<int>(kap::kpl::Statement::Kind::Let));
});

KAP_TEST("KPL parser accepts schema types, match arms, and inline conditionals")
{
    const auto plugin =
        kap::kpl::parse("schema { mode: enum { auto, ninja } = auto args: list<str> = [] }\n"
                        "command pick(config) {\n"
                        "  let value = if config.release then \"yes\" else \"no\"\n"
                        "  match config.mode { auto => value, ninja => \"fast\", }\n"
                        "}\n");
    KAP_ASSERT(plugin.schema.has_value());
    KAP_ASSERT_EQ(plugin.commands[0].body.statements.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(static_cast<int>(plugin.commands[0].body.statements[1].kind),
                  static_cast<int>(kap::kpl::Statement::Kind::Match));
});

KAP_TEST("KPL parser rejects malformed declarations with a location")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::parse("command build( { }\n", "plugin.kpl"));
});

KAP_TEST("KPL manifest validation accepts the supported contract")
{
    const auto plugin = kap::kpl::parse(
        "manifest { name = \"demo\" version = \"1.0.0\" api_version = 1 }\n");
    KAP_ASSERT(kap::kpl::validate(plugin).empty());
});

KAP_TEST("KPL manifest validation reports missing and newer fields")
{
    const auto plugin = kap::kpl::parse(
        "manifest { name = 1 api_version = 2 }\n");
    const auto errors = kap::kpl::validate(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(3));
    KAP_ASSERT(errors[0].find("newer") != std::string::npos);
    KAP_ASSERT(errors[1].find("name") != std::string::npos);
    KAP_ASSERT(errors[2].find("version") != std::string::npos);
});
