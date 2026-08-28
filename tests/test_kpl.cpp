// tests/test_kpl.cpp
//
// Lexer contract for the KPL front-end (design doc Milestone 2).

#include "core/diag.hpp"
#include "core/kpl.hpp"
#include "harness.hpp"

#include <cstddef>

KAP_TEST("KPL lexer handles comments, literals, and operators")
{
    const auto tokens = kap::kpl::lex(
        "// header\nmanifest { name = \"cmake-cpp\" priority = -2 }\n"
        "if a && b != 3 { step [\"cmake\"] + extra }\n");

    KAP_ASSERT_EQ(tokens[0].text, "manifest");
    KAP_ASSERT_EQ(static_cast<int>(tokens[1].kind),
                  static_cast<int>(kap::kpl::TokenKind::LeftBrace));
    KAP_ASSERT_EQ(static_cast<int>(tokens[3].kind),
                  static_cast<int>(kap::kpl::TokenKind::Equal));
    KAP_ASSERT_EQ(tokens[4].text, "cmake-cpp");
    KAP_ASSERT_EQ(static_cast<int>(tokens[7].kind),
                  static_cast<int>(kap::kpl::TokenKind::Minus));
    KAP_ASSERT_EQ(tokens[8].integer, static_cast<std::int64_t>(2));
    KAP_ASSERT_EQ(static_cast<int>(tokens[12].kind),
                  static_cast<int>(kap::kpl::TokenKind::AndAnd));
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
