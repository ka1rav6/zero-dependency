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
    const auto plugin =
        kap::kpl::parse("manifest { name = \"demo\" version = \"1.0.0\" api_version = 1 }\n");
    KAP_ASSERT(kap::kpl::validate(plugin).empty());
});

KAP_TEST("KPL manifest validation reports missing and newer fields")
{
    const auto plugin = kap::kpl::parse("manifest { name = 1 api_version = 2 }\n");
    const auto errors = kap::kpl::validate(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(3));
    KAP_ASSERT(errors[0].find("newer") != std::string::npos);
    KAP_ASSERT(errors[1].find("name") != std::string::npos);
    KAP_ASSERT(errors[2].find("version") != std::string::npos);
});

KAP_TEST("KPL evaluator builds steps from config, project, and extra args")
{
    const auto              plugin = kap::kpl::parse("command build(project, config, extra) {"
                                                     " let dir = config.build_dir"
                                                     " if project.tool(\"ninja\") { step \"ninja\" dir }"
                                                     " else { step \"make\" dir }"
                                                     " step [\"test\"] + extra"
                                                     "}");
    const kap::kpl::Project project{.root          = "/tmp/project",
                                    .matched_files = {"CMakeLists.txt"},
                                    .exists        = {},
                                    .tool = [](std::string_view name) { return name == "ninja"; }};
    const auto              spec = kap::kpl::evaluate(plugin,
                                         "build",
                                         project,
                                                      {{"build_dir", kap::kpl::Value::string_value("out")}},
                                                      {"--release"});

    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("ninja"));
    KAP_ASSERT_EQ(spec.steps[0].command[1], std::string("out"));
    KAP_ASSERT_EQ(spec.steps[1].command[0], std::string("test"));
    KAP_ASSERT_EQ(spec.steps[1].command[1], std::string("--release"));
});

KAP_TEST("KPL evaluator applies concurrent and freed-space modifiers")
{
    const auto plugin =
        kap::kpl::parse("command clean(project, config) { concurrent true step \"rm\" \"-rf\""
                        " config.build_dir report_freed_space }");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(
        plugin, "clean", project, {{"build_dir", kap::kpl::Value::string_value("build")}});

    KAP_ASSERT(spec.concurrent);
    KAP_ASSERT(spec.report_freed_space);
    KAP_ASSERT_EQ(spec.steps[0].command[2], std::string("build"));
});

KAP_TEST("KPL schema builds typed defaults and accepts overrides")
{
    const auto plugin = kap::kpl::parse(
        "schema { mode: enum { auto, ninja } = auto retries: int = 2 "
        "release: bool = false args: list<str> = [] }");
    const auto fields = kap::kpl::schema(plugin);
    KAP_ASSERT_EQ(fields.size(), static_cast<std::size_t>(4));
    KAP_ASSERT_EQ(fields[3].type, "list<str>");

    const auto [config, errors] = kap::kpl::build_config(
        plugin, {{"mode", kap::kpl::Value::string_value("ninja")},
                 {"release", kap::kpl::Value::boolean_value(true)}});
    KAP_ASSERT(errors.empty());
    KAP_ASSERT_EQ(config.at("mode").string, "ninja");
    KAP_ASSERT_EQ(config.at("retries").integer, static_cast<std::int64_t>(2));
    KAP_ASSERT(config.at("release").boolean);
});

KAP_TEST("KPL schema rejects unknown and incorrectly typed config")
{
    const auto plugin = kap::kpl::parse("schema { release: bool = false }");
    const auto [config, errors] = kap::kpl::build_config(
        plugin, {{"release", kap::kpl::Value::string_value("yes")},
                 {"unknown", kap::kpl::Value::integer_value(1)}});
    KAP_ASSERT(config.at("release").boolean == false);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(2));
    KAP_ASSERT(errors[0].find("unknown config key") != std::string::npos ||
               errors[1].find("unknown config key") != std::string::npos);
});

KAP_TEST("KPL type checker accepts valid command expressions")
{
    const auto plugin = kap::kpl::parse(
        "command build(project, config, extra) {"
        " let args = [\"cmake\"] + extra"
        " if project.tool(\"ninja\") { step args }"
        " concurrent config.release"
        "}");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());
});

KAP_TEST("KPL type checker rejects invalid conditions and step values")
{
    const auto plugin = kap::kpl::parse(
        "command broken(config) { if \"yes\" { step 42 } concurrent 1 }");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(3));
    KAP_ASSERT(errors[0].find("boolean") != std::string::npos);
    KAP_ASSERT(errors[1].find("strings") != std::string::npos);
    KAP_ASSERT(errors[2].find("boolean") != std::string::npos);
});
