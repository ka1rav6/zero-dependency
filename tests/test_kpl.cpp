// tests/test_kpl.cpp
//
// Lexer contract for the KPL front-end (design doc Milestone 2).

#include "core/diag.hpp"
#include "core/kpl.hpp"
#include "harness.hpp"

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace
{

// A throwaway project tree unique to this process, used by the host_project
// tests below. Mirrors the helper in tests/test_fs.cpp; kept local rather than
// shared so a change to one file's fixtures cannot break the other's.
std::filesystem::path scratch_project(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("kap_kpl_" + name + "_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

} // namespace

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
    const auto        plugin = kap::kpl::parse("command build(project, config, extra) {"
                                               " let dir = config.build_dir"
                                               " if project.tool(\"ninja\") { step \"ninja\" dir }"
                                               " else { step \"make\" dir }"
                                               " step [\"test\"] + extra"
                                               "}");
    kap::kpl::Project project;
    project.root          = "/tmp/project";
    project.matched_files = {"CMakeLists.txt"};
    project.tool          = [](std::string_view name) { return name == "ninja"; };
    const auto spec       = kap::kpl::evaluate(plugin,
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

KAP_TEST("KPL fail stops the command and records the reason")
{
    const auto plugin =
        kap::kpl::parse("command build(project, config) { step \"first\" fail \"nothing to build\""
                        " step \"unreachable\" }");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(plugin, "build", project);

    KAP_ASSERT(spec.failure.has_value());
    KAP_ASSERT_EQ(*spec.failure, std::string("nothing to build"));
    // Steps before the fail are kept for --dry-run; the one after it never ran.
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("first"));
});

KAP_TEST("KPL fail unwinds out of nested blocks")
{
    const auto plugin = kap::kpl::parse(
        "command build(project, config) { for name in config.names { if name == \"stop\""
        " { fail \"stopped at \" + name } step name } }");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(
        plugin,
        "build",
        project,
        {{"names",
                       kap::kpl::Value::list_value({kap::kpl::Value::string_value("go"),
                                                    kap::kpl::Value::string_value("stop"),
                                                    kap::kpl::Value::string_value("never")})}});

    KAP_ASSERT(spec.failure.has_value());
    KAP_ASSERT_EQ(*spec.failure, std::string("stopped at stop"));
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("go"));
});

KAP_TEST("KPL fail requires a string message")
{
    const auto plugin = kap::kpl::parse("command build(project, config) { fail 42 }");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT(!errors.empty());
});

KAP_TEST("KPL failure round-trips through the CommandSpec JSON contract")
{
    kap::kpl::CommandSpec spec;
    spec.failure        = "no 'build' script";
    const auto restored = kap::kpl::spec_from_json(kap::kpl::to_json(spec), "test");
    KAP_ASSERT(restored.failure.has_value());
    KAP_ASSERT_EQ(*restored.failure, std::string("no 'build' script"));

    // Absent means absent, not an empty message.
    kap::kpl::CommandSpec plain;
    KAP_ASSERT(!kap::kpl::spec_from_json(kap::kpl::to_json(plain), "test").failure.has_value());
});

KAP_TEST("KPL schema builds typed defaults and accepts overrides")
{
    const auto plugin =
        kap::kpl::parse("schema { mode: enum { auto, ninja } = auto retries: int = 2 "
                        "release: bool = false args: list<str> = [] }");
    const auto fields = kap::kpl::schema(plugin);
    KAP_ASSERT_EQ(fields.size(), static_cast<std::size_t>(4));
    KAP_ASSERT_EQ(fields[3].type, "list<str>");

    const auto [config, errors] =
        kap::kpl::build_config(plugin,
                               {{"mode", kap::kpl::Value::string_value("ninja")},
                                {"release", kap::kpl::Value::boolean_value(true)}});
    KAP_ASSERT(errors.empty());
    KAP_ASSERT_EQ(config.at("mode").string, "ninja");
    KAP_ASSERT_EQ(config.at("retries").integer, static_cast<std::int64_t>(2));
    KAP_ASSERT(config.at("release").boolean);
});

KAP_TEST("KPL schema rejects unknown and incorrectly typed config")
{
    const auto plugin = kap::kpl::parse("schema { release: bool = false }");
    const auto [config, errors] =
        kap::kpl::build_config(plugin,
                               {{"release", kap::kpl::Value::string_value("yes")},
                                {"unknown", kap::kpl::Value::integer_value(1)}});
    KAP_ASSERT(config.at("release").boolean == false);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(2));
    KAP_ASSERT(errors[0].find("unknown config key") != std::string::npos ||
               errors[1].find("unknown config key") != std::string::npos);
});

KAP_TEST("KPL type checker accepts valid command expressions")
{
    const auto plugin = kap::kpl::parse("command build(project, config, extra) {"
                                        " let args = [\"cmake\"] + extra"
                                        " if project.tool(\"ninja\") { step args }"
                                        " concurrent config.release"
                                        "}");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());
});

KAP_TEST("KPL type checker rejects invalid conditions and step values")
{
    const auto plugin =
        kap::kpl::parse("command broken(config) { if \"yes\" { step 42 } concurrent 1 }");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(3));
    KAP_ASSERT(errors[0].find("boolean") != std::string::npos);
    KAP_ASSERT(errors[1].find("strings") != std::string::npos);
    KAP_ASSERT(errors[2].find("boolean") != std::string::npos);
});

// --- Regression tests: operator semantics ----------------------------------------

KAP_TEST("KPL evaluator returns a value from non-short-circuiting && and ||")
{
    // Regression: `&&`/`||` used to return early only on the short-circuit
    // path and then fall through to the arithmetic ladder, so every logical
    // expression that actually had to look at its right operand died with
    // "incompatible operands".
    const auto              plugin = kap::kpl::parse("command build(project, config, extra) {"
                                                     " if true && true { step \"both\" }"
                                                     " if false || true { step \"either\" }"
                                                     " if true && false { step \"unreachable\" }"
                                                     "}");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(plugin, "build", project);

    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("both"));
    KAP_ASSERT_EQ(spec.steps[1].command[0], std::string("either"));
});

KAP_TEST("KPL evaluator short-circuits && and || without evaluating the right side")
{
    // `project.tool` is the only observable side effect available here: if the
    // right operand were evaluated, the missing-tool lookup would still run.
    // Counting lookups proves the short circuit actually happened.
    int               lookups = 0;
    const auto        plugin  = kap::kpl::parse("command build(project, config, extra) {"
                                                " if false && project.tool(\"ninja\") { step \"no\" }"
                                                " if true || project.tool(\"ninja\") { step \"yes\" }"
                                                "}");
    kap::kpl::Project project;
    project.root = "/tmp";
    project.tool = [&lookups](std::string_view) {
        ++lookups;
        return true;
    };
    const auto spec = kap::kpl::evaluate(plugin, "build", project);

    KAP_ASSERT_EQ(lookups, 0);
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
});

KAP_TEST("KPL evaluator rejects non-boolean logical operands")
{
    const auto plugin = kap::kpl::parse(
        "command build(project, config, extra) { if \"yes\" && true { step \"x\" } }");
    const kap::kpl::Project project{};
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(plugin, "build", project));
});

KAP_TEST("KPL equality compares lists and records structurally")
{
    // Regression: equality used to compare Value's scalar fields side by side,
    // which never looked at list elements — so any two lists were "equal".
    const auto              plugin = kap::kpl::parse("command build(project, config, extra) {"
                                                     " if [\"a\"] == [\"b\"] { step \"wrong\" }"
                                                     " if [\"a\"] == [\"a\"] { step \"same\" }"
                                                     " if [\"a\"] != [\"a\", \"b\"] { step \"length\" }"
                                                     " if 1 == \"1\" { step \"crosstype\" }"
                                                     " if none == none { step \"none\" }"
                                                     "}");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(plugin, "build", project);

    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("same"));
    KAP_ASSERT_EQ(spec.steps[1].command[0], std::string("length"));
    KAP_ASSERT_EQ(spec.steps[2].command[0], std::string("none"));
});

// --- Regression tests: diagnostics -----------------------------------------------

KAP_TEST("KPL run-time errors name the plugin source file")
{
    // Regression: the evaluator's source name was never initialised, so every
    // run-time diagnostic rendered as "<unknown>:line:col" no matter which
    // plugin raised it. The name now rides on the AST.
    const auto              plugin = kap::kpl::parse("command build(project, config, extra) {\n"
                                                     "  step nope.missing\n"
                                                     "}\n",
                                        "cmake-cpp/plugin.kpl");
    const kap::kpl::Project project{};
    try {
        kap::kpl::evaluate(plugin, "build", project);
        KAP_ASSERT(false);
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT(error.report().find("cmake-cpp/plugin.kpl:2:") != std::string::npos);
    }
});

KAP_TEST("KPL inline conditional keeps its source location")
{
    // Regression: the parser built the Conditional node out of an object it
    // had already moved from, losing the token — and with it the line/column
    // every diagnostic about that node needs.
    const auto              plugin = kap::kpl::parse("command build(project, config, extra) {\n"
                                                     "  let x = \"s\" then \"a\" else \"b\"\n"
                                                     "  step x\n"
                                                     "}\n",
                                        "plugin.kpl");
    const kap::kpl::Project project{};
    try {
        kap::kpl::evaluate(plugin, "build", project);
        KAP_ASSERT(false);
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT_EQ(error.diagnostic().location.line, 2);
        KAP_ASSERT(error.diagnostic().location.col > 0);
    }
});

KAP_TEST("KPL commands only see the parameters they declare")
{
    // Regression: the environment was pre-loaded with project/config/extra
    // regardless of the signature, so a command could read a host value it
    // never asked for.
    // The probe is `let`, not `step`: a lone identifier in step position is a
    // bare program word (see the bare-word tests below), so it would not
    // surface an unbound name.
    const auto plugin = kap::kpl::parse("command clean(project, config) { let e = extra step e }");
    const kap::kpl::Project project{};
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(plugin, "clean", project, {}, {"x"}));

    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("extra") != std::string::npos);
});

KAP_TEST("KPL rejects a command parameter that is not a host value")
{
    const auto plugin = kap::kpl::parse("command build(project, widget) { step \"x\" }");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("widget") != std::string::npos);

    const kap::kpl::Project project{};
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(plugin, "build", project));
});

// --- Regression tests: static typing ---------------------------------------------

KAP_TEST("KPL type checker keeps the element type across list concatenation")
{
    // Regression: `+` used to answer "integer" whenever either operand was
    // Unknown, so the design doc's own idiom — `["cmake"] + config.cmake_args`
    // — was rejected with "step arguments must be strings, got integer".
    const auto plugin = kap::kpl::parse("schema { cmake_args: list<str> = [] }\n"
                                        "command build(project, config, extra) {\n"
                                        "  let cmd = [\"cmake\"] + config.cmake_args + extra\n"
                                        "  step cmd\n"
                                        "}\n");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());
});

KAP_TEST("KPL type checker types config fields from the schema block")
{
    const auto plugin = kap::kpl::parse("schema { build_dir: str = \"build\" jobs: int = 4 }\n"
                                        "command build(project, config) {\n"
                                        "  step \"make\" config.build_dir\n"
                                        "  if config.jobs > 1 { step \"parallel\" }\n"
                                        "}\n");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());
});

KAP_TEST("KPL type checker rejects a config key missing from the schema")
{
    const auto plugin =
        kap::kpl::parse("schema { build_dir: str = \"build\" }\n"
                        "command build(project, config) { step config.buidl_dir }\n");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("buidl_dir") != std::string::npos);
});

KAP_TEST("KPL type checker leaves config unchecked when a plugin has no schema")
{
    // A plugin with no schema block declares no config surface, so there is
    // nothing to check the read against; it stays Unknown rather than becoming
    // a false "unknown config key".
    const auto plugin = kap::kpl::parse("command build(project, config) { step config.anything }");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());
});

KAP_TEST("KPL type checker rejects mistyped schema-backed expressions")
{
    const auto plugin = kap::kpl::parse("schema { build_dir: str = \"build\" jobs: int = 4 }\n"
                                        "command build(project, config) {\n"
                                        "  step \"x\" + config.jobs\n"
                                        "  if config.build_dir > 1 { step \"y\" }\n"
                                        "}\n");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(2));
    KAP_ASSERT(errors[0].find("cannot add") != std::string::npos);
    KAP_ASSERT(errors[1].find("comparison requires integers") != std::string::npos);
});

KAP_TEST("KPL type checker types the loop variable from the list element type")
{
    const auto plugin = kap::kpl::parse("schema { jobs: list<int> = [] }\n"
                                        "command build(project, config, extra) {\n"
                                        "  for word in extra { step \"echo\" word }\n"
                                        "  for job in config.jobs { step job }\n"
                                        "}\n");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("integer") != std::string::npos);
});

KAP_TEST("KPL type checker rejects iterating a non-list")
{
    const auto plugin =
        kap::kpl::parse("command build(project) { for c in project.root { step c } }");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("for loop requires a list") != std::string::npos);
});

// --- Host builtins (design doc §5.8) ---------------------------------------------

KAP_TEST("KPL evaluates host call arguments as expressions")
{
    // Regression: the evaluator read the argument off the AST and demanded a
    // literal, so the design doc's own workspace idiom
    // `project.exists(ws + "/package.json")` was rejected.
    const auto        plugin = kap::kpl::parse("command build(project, config, extra) {"
                                               "  let dir = \"packages/app\""
                                               "  if project.exists(dir + \"/package.json\") {"
                                               "    step \"npm\" \"run\" \"build\""
                                               "  }"
                                               "}");
    kap::kpl::Project project;
    project.exists = [](std::string_view path) { return path == "packages/app/package.json"; };

    const auto spec = kap::kpl::evaluate(plugin, "build", project);
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("npm"));
});

KAP_TEST("KPL exposes project.read, project.glob, and project.env")
{
    const auto        plugin = kap::kpl::parse("command build(project, config, extra) {"
                                               "  step \"read\" trim(project.read(\"VERSION\"))"
                                               "  step [\"glob\"] + project.glob(\"packages/*\")"
                                               "  if project.env(\"CI\") != none {"
                                               "    step \"ci\" project.env(\"CI\")"
                                               "  }"
                                               "  if project.env(\"MISSING\") == none { step \"unset\" }"
                                               "}");
    kap::kpl::Project project;
    project.read = [](std::string_view) { return std::string("  1.2.3\n"); };
    project.glob = [](std::string_view) {
        return std::vector<std::string>{"packages/a", "packages/b"};
    };
    project.env = [](std::string_view name) -> std::optional<std::string> {
        if (name == "CI")
            return std::string("true");
        return std::nullopt;
    };

    const auto spec = kap::kpl::evaluate(plugin, "build", project);
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(4));
    KAP_ASSERT_EQ(spec.steps[0].command[1], std::string("1.2.3"));
    KAP_ASSERT_EQ(spec.steps[1].command.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(spec.steps[1].command[2], std::string("packages/b"));
    KAP_ASSERT_EQ(spec.steps[2].command[1], std::string("true"));
    KAP_ASSERT_EQ(spec.steps[3].command[0], std::string("unset"));
});

KAP_TEST("KPL stdlib provides len, contains, trim, and split")
{
    const auto              plugin = kap::kpl::parse("command build(project, config, extra) {"
                                                     "  if len(extra) == 2 { step \"two\" }"
                                                     "  if contains(\"a-b-c\", \"-b-\") { step \"contains\" }"
                                                     "  step \"trim\" trim(\"\\t pad \\n\")"
                                                     "  step [\"split\"] + split(\"a:b:c\", \":\")"
                                                     "}");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(plugin, "build", project, {}, {"x", "y"});

    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(4));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("two"));
    KAP_ASSERT_EQ(spec.steps[1].command[0], std::string("contains"));
    KAP_ASSERT_EQ(spec.steps[2].command[1], std::string("pad"));
    KAP_ASSERT_EQ(spec.steps[3].command.size(), static_cast<std::size_t>(4));
    KAP_ASSERT_EQ(spec.steps[3].command[3], std::string("c"));
});

KAP_TEST("KPL rejects unknown functions and wrong call arities")
{
    const auto plugin = kap::kpl::parse("command build(project, config, extra) {"
                                        "  step trim(\"a\", \"b\")"
                                        "  step upper(\"a\")"
                                        "  step project.explode(\"a\")"
                                        "  if len(\"not a list\") == 0 { step \"x\" }"
                                        "}");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(4));
    KAP_ASSERT(errors[0].find("takes 1 argument") != std::string::npos);
    KAP_ASSERT(errors[1].find("unknown function 'upper'") != std::string::npos);
    KAP_ASSERT(errors[2].find("unknown project method 'explode'") != std::string::npos);
    KAP_ASSERT(errors[3].find("must be list") != std::string::npos);
});

KAP_TEST("KPL reports missing host capabilities instead of guessing")
{
    // An unset `read` callback cannot answer "" — that is indistinguishable
    // from an empty file — so it raises. Query-shaped capabilities degrade
    // safely instead.
    const auto reader = kap::kpl::parse("command build(project) { step project.read(\"x\") }");
    const auto querier =
        kap::kpl::parse("command build(project) { if !project.exists(\"x\") { step \"absent\" } }");
    const kap::kpl::Project project{};

    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(reader, "build", project));
    const auto spec = kap::kpl::evaluate(querier, "build", project);
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
});

// --- The production host object (design doc §3.4 + §7) ---------------------------

KAP_TEST("host_project reads, globs, and probes inside the project root")
{
    const std::filesystem::path root = scratch_project("host");
    write_file(root / "VERSION", "1.2.3\n");
    write_file(root / "packages" / "app" / "package.json", "{}");
    write_file(root / "packages" / "lib" / "package.json", "{}");

    const kap::kpl::Project project = kap::kpl::host_project(root.string(), {"VERSION"});

    KAP_ASSERT(project.exists("VERSION"));
    KAP_ASSERT(project.exists("packages/app"));
    KAP_ASSERT(!project.exists("nope"));
    KAP_ASSERT_EQ(project.read("VERSION"), std::string("1.2.3\n"));

    const std::vector<std::string> workspaces = project.glob("packages/*");
    KAP_ASSERT_EQ(workspaces.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(workspaces[0], std::string("packages/app"));
    KAP_ASSERT(project.exists(workspaces[1] + "/package.json"));

    std::filesystem::remove_all(root);
});

KAP_TEST("host_project refuses paths that escape the project root")
{
    const std::filesystem::path root = scratch_project("escape");
    write_file(root.parent_path() / "kap_outside_secret.txt", "secret");

    const kap::kpl::Project project = kap::kpl::host_project(root.string());

    KAP_ASSERT(!project.exists("../kap_outside_secret.txt"));
    KAP_ASSERT(!project.exists("/etc/passwd"));
    KAP_ASSERT_THROWS(kap::diag::Error, project.read("../kap_outside_secret.txt"));
    KAP_ASSERT(project.glob("../*").empty());

    std::filesystem::remove_all(root);
    std::filesystem::remove(root.parent_path() / "kap_outside_secret.txt");
});

KAP_TEST("host_project filters secret-shaped environment variables")
{
    // Design doc §7: a deny-list, so ordinary build variables stay readable
    // while anything shaped like a credential does not.
    ::setenv("KAP_TEST_PLAIN", "visible", 1);
    ::setenv("KAP_TEST_API_TOKEN", "secret", 1);
    ::setenv("KAP_TEST_SIGNING_KEY", "secret", 1);
    ::setenv("AWS_ACCESS_KEY_ID", "secret", 1);

    const kap::kpl::Project project = kap::kpl::host_project(".");

    KAP_ASSERT(project.env("KAP_TEST_PLAIN").has_value());
    KAP_ASSERT_EQ(*project.env("KAP_TEST_PLAIN"), std::string("visible"));
    KAP_ASSERT(!project.env("KAP_TEST_API_TOKEN").has_value());
    KAP_ASSERT(!project.env("KAP_TEST_SIGNING_KEY").has_value());
    KAP_ASSERT(!project.env("AWS_ACCESS_KEY_ID").has_value());
    KAP_ASSERT(!project.env("KAP_TEST_UNSET_VARIABLE").has_value());

    ::unsetenv("KAP_TEST_PLAIN");
    ::unsetenv("KAP_TEST_API_TOKEN");
    ::unsetenv("KAP_TEST_SIGNING_KEY");
    ::unsetenv("AWS_ACCESS_KEY_ID");
});

KAP_TEST("host_project resolves tools on PATH without executing them")
{
    const kap::kpl::Project project = kap::kpl::host_project(".");

    // /bin/sh is mandated by POSIX, so `sh` is on PATH anywhere these tests
    // can run at all.
    KAP_ASSERT(project.tool("sh"));
    KAP_ASSERT(!project.tool("kap_definitely_not_a_real_tool"));
    // A name containing a path separator is not a PATH lookup and is refused
    // rather than probed.
    KAP_ASSERT(!project.tool("/bin/sh"));
});

// --- Control flow: for (design doc §5.9) -----------------------------------------

KAP_TEST("KPL for iterates a list and emits one step per element")
{
    const auto        plugin = kap::kpl::parse("command dev(project, config, extra) {"
                                               "  concurrent true"
                                               "  for ws in project.glob(\"packages/*\") {"
                                               "    if project.exists(ws + \"/package.json\") {"
                                               "      step \"npm\" \"run\" \"dev\" ws"
                                               "    }"
                                               "  }"
                                               "}");
    kap::kpl::Project project;
    project.glob = [](std::string_view) {
        return std::vector<std::string>{"packages/app", "packages/docs", "packages/scratch"};
    };
    project.exists = [](std::string_view path) { return path != "packages/scratch/package.json"; };

    const auto spec = kap::kpl::evaluate(plugin, "dev", project);
    KAP_ASSERT(spec.concurrent);
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(spec.steps[0].command[3], std::string("packages/app"));
    KAP_ASSERT_EQ(spec.steps[1].command[3], std::string("packages/docs"));
});

KAP_TEST("KPL for over an empty list runs its body zero times")
{
    const auto plugin =
        kap::kpl::parse("command dev(project, extra) { for w in extra { step \"echo\" w } }");
    const kap::kpl::Project project{};
    KAP_ASSERT(kap::kpl::evaluate(plugin, "dev", project, {}, {}).steps.empty());
});

KAP_TEST("KPL scopes the for loop variable to the loop")
{
    const auto plugin = kap::kpl::parse(
        "command dev(project, extra) { for w in extra { step w } let after = w step after }");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("unknown name 'w'") != std::string::npos);

    const kap::kpl::Project project{};
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(plugin, "dev", project, {}, {"a"}));
});

KAP_TEST("KPL for restores an outer binding of the same name")
{
    const auto              plugin = kap::kpl::parse("command dev(project, extra) {"
                                                     "  let w = \"outer\""
                                                     "  for w in extra { step w }"
                                                     "  step w"
                                                     "}");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(plugin, "dev", project, {}, {"a", "b"});
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(spec.steps[2].command[0], std::string("outer"));
});

KAP_TEST("KPL for rejects a non-list at run time")
{
    const auto plugin =
        kap::kpl::parse("command dev(project) { for c in project.root { step c } }");
    kap::kpl::Project project;
    project.root = "/tmp";
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(plugin, "dev", project));
});

// --- Control flow: match (design doc §5.9) ---------------------------------------

KAP_TEST("KPL match is an expression whose arms select a value")
{
    // This is design doc §5.3's cmake-cpp generator selection, which could not
    // even be parsed before: `match` existed only as a statement.
    const auto plugin =
        kap::kpl::parse("schema { generator: enum { auto, ninja, make } = auto }\n"
                        "command build(project, config) {\n"
                        "  let gen = match config.generator {\n"
                        "    auto  => if project.tool(\"ninja\") then \"Ninja\" else none,\n"
                        "    ninja => \"Ninja\",\n"
                        "    make  => \"Unix Makefiles\",\n"
                        "  }\n"
                        "  if gen != none { step \"cmake\" \"-G\" gen }\n"
                        "}\n");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());

    kap::kpl::Project project;
    project.tool = [](std::string_view name) { return name == "ninja"; };

    const auto [defaults, errors] = kap::kpl::build_config(plugin, {});
    KAP_ASSERT(errors.empty());
    const auto automatic = kap::kpl::evaluate(plugin, "build", project, defaults);
    KAP_ASSERT_EQ(automatic.steps.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(automatic.steps[0].command[2], std::string("Ninja"));

    const auto [pinned, pin_errors] =
        kap::kpl::build_config(plugin, {{"generator", kap::kpl::Value::string_value("make")}});
    KAP_ASSERT(pin_errors.empty());
    const auto explicit_make = kap::kpl::evaluate(plugin, "build", project, pinned);
    KAP_ASSERT_EQ(explicit_make.steps[0].command[2], std::string("Unix Makefiles"));
});

KAP_TEST("KPL match falls through to a none arm and drops the step")
{
    const auto plugin =
        kap::kpl::parse("schema { generator: enum { auto, ninja } = auto }\n"
                        "command build(project, config) {\n"
                        "  let gen = match config.generator {\n"
                        "    auto  => if project.tool(\"ninja\") then \"Ninja\" else none,\n"
                        "    ninja => \"Ninja\",\n"
                        "  }\n"
                        "  if gen != none { step \"cmake\" \"-G\" gen }\n"
                        "  step \"cmake\" \"--build\" \"build\"\n"
                        "}\n");
    kap::kpl::Project project;
    project.tool = [](std::string_view) { return false; };

    const auto [defaults, errors] = kap::kpl::build_config(plugin, {});
    KAP_ASSERT(errors.empty());
    const auto spec = kap::kpl::evaluate(plugin, "build", project, defaults);
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(spec.steps[0].command[1], std::string("--build"));
});

KAP_TEST("KPL match checks enum exhaustiveness against the schema")
{
    const auto plugin = kap::kpl::parse("schema { mode: enum { fast, slow, safe } = fast }\n"
                                        "command build(project, config) {\n"
                                        "  step match config.mode { fast => \"-O2\", }\n"
                                        "}\n");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("does not cover slow, safe") != std::string::npos);
});

KAP_TEST("KPL match matches literal patterns structurally")
{
    const auto              plugin = kap::kpl::parse("command build(project, extra) {"
                                                     "  step match len(extra) { 0 => \"none\", 1 => \"one\", }"
                                                     "}");
    const kap::kpl::Project project{};
    KAP_ASSERT_EQ(kap::kpl::evaluate(plugin, "build", project, {}, {}).steps[0].command[0],
                  std::string("none"));
    KAP_ASSERT_EQ(kap::kpl::evaluate(plugin, "build", project, {}, {"a"}).steps[0].command[0],
                  std::string("one"));
});

KAP_TEST("KPL match reports an uncovered value at run time")
{
    const auto              plugin = kap::kpl::parse("command build(project, extra) {\n"
                                                     "  step match len(extra) { 0 => \"none\", }\n"
                                                     "}\n",
                                        "plugin.kpl");
    const kap::kpl::Project project{};
    try {
        kap::kpl::evaluate(plugin, "build", project, {}, {"a", "b"});
        KAP_ASSERT(false);
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT(error.report().find("no match arm covers 2") != std::string::npos);
        KAP_ASSERT(error.report().find("plugin.kpl:2:") != std::string::npos);
    }
});

KAP_TEST("KPL match requires at least one arm")
{
    KAP_ASSERT_THROWS(kap::diag::Error,
                      kap::kpl::parse("command build(config) { step match config.mode { } }"));
});

// --- The record step form (design doc §5.4) --------------------------------------

KAP_TEST("KPL record steps carry cwd, label, and env")
{
    // Design doc §5.11's workspace `dev`: the record form is the only way to
    // reach a step's cwd and label, which the executor needs for concurrent
    // prefixed output.
    const auto plugin =
        kap::kpl::parse("command dev(project, config, extra) {"
                        "  concurrent true"
                        "  for ws in project.glob(\"packages/*\") {"
                        "    step { cmd: [\"npm\", \"run\", \"dev\"], cwd: ws, label: ws,"
                        "           env: { NODE_ENV: \"development\" } }"
                        "  }"
                        "}");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());

    kap::kpl::Project project;
    project.glob = [](std::string_view) {
        return std::vector<std::string>{"packages/api", "packages/web"};
    };

    const auto spec = kap::kpl::evaluate(plugin, "dev", project);
    KAP_ASSERT(spec.concurrent);
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(spec.steps[0].command.size(), static_cast<std::size_t>(3));
    KAP_ASSERT(spec.steps[0].cwd.has_value());
    KAP_ASSERT_EQ(*spec.steps[0].cwd, std::string("packages/api"));
    KAP_ASSERT(spec.steps[1].label.has_value());
    KAP_ASSERT_EQ(*spec.steps[1].label, std::string("packages/web"));
    KAP_ASSERT_EQ(spec.steps[0].environment.at("NODE_ENV"), std::string("development"));
});

KAP_TEST("KPL record steps default cwd, label, and env to unset")
{
    const auto plugin =
        kap::kpl::parse("command build(project) { step { cmd: [\"cmake\", \"--build\", \".\"] } }");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(plugin, "build", project);

    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(!spec.steps[0].cwd.has_value());
    KAP_ASSERT(!spec.steps[0].label.has_value());
    KAP_ASSERT(spec.steps[0].environment.empty());
});

KAP_TEST("KPL rejects malformed record steps")
{
    const auto misspelled =
        kap::kpl::parse("command build(project) { step { cmd: [\"x\"], cdw: \"y\" } }");
    const auto errors = kap::kpl::type_check(misspelled);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("unknown step field 'cdw'") != std::string::npos);

    const auto no_command = kap::kpl::parse("command build(project) { step { cwd: \"y\" } }");
    KAP_ASSERT(!kap::kpl::type_check(no_command).empty());
    const kap::kpl::Project project{};
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(no_command, "build", project));

    const auto mixed = kap::kpl::parse("command build(project) { step \"a\" { cmd: [\"x\"] } }");
    KAP_ASSERT(!kap::kpl::type_check(mixed).empty());
});

// --- Bare-word step arguments (design doc §5.3 / §5.4) ---------------------------

KAP_TEST("KPL treats an unbound identifier in step position as a bare word")
{
    // Design doc §5.3 writes `step mkdir "-p" dir`: `mkdir` is a program name
    // and `dir` is a variable, both bare identifiers. The rule that makes both
    // work is "bound name wins, otherwise the literal word".
    const auto plugin = kap::kpl::parse("command build(project, config) {"
                                        "  let dir = config.build_dir"
                                        "  step mkdir \"-p\" dir"
                                        "  step rm \"-rf\" config.build_dir"
                                        "}");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());

    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(
        plugin, "build", project, {{"build_dir", kap::kpl::Value::string_value("out")}});

    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(spec.steps[0].command[0], std::string("mkdir"));
    KAP_ASSERT_EQ(spec.steps[0].command[2], std::string("out"));
    KAP_ASSERT_EQ(spec.steps[1].command[0], std::string("rm"));
    KAP_ASSERT_EQ(spec.steps[1].command[2], std::string("out"));
});

KAP_TEST("KPL bare words do not apply inside a larger expression")
{
    // The rule is confined to a lone identifier directly in step position, so
    // a genuinely unbound name used in an expression is still an error.
    const auto in_list   = kap::kpl::parse("command build(project) { step [mkdir] }");
    const auto in_concat = kap::kpl::parse("command build(project) { step mkdir + \"x\" }");

    KAP_ASSERT(!kap::kpl::type_check(in_list).empty());
    KAP_ASSERT(!kap::kpl::type_check(in_concat).empty());

    const kap::kpl::Project project{};
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(in_list, "build", project));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(in_concat, "build", project));
});

KAP_TEST("KPL prefers a bound variable over a bare word of the same name")
{
    const auto              plugin = kap::kpl::parse("command build(project) {"
                                                     "  let cmake = [\"cmake\", \"--build\", \"build\"]"
                                                     "  step cmake"
                                                     "}");
    const kap::kpl::Project project{};
    const auto              spec = kap::kpl::evaluate(plugin, "build", project);

    KAP_ASSERT_EQ(spec.steps[0].command.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(spec.steps[0].command[1], std::string("--build"));
});

// --- Regression tests: schema, records, and assignment ---------------------------

KAP_TEST("KPL schema accepts a negative integer default")
{
    // Regression: the lexer emits '-' as its own token, so `-1` reaches the
    // schema reader as a unary node. Without a case for it the field looked
    // like it had no default and the whole plugin was rejected.
    const auto plugin = kap::kpl::parse("schema { retries: int = -1 offset: int = 0 }");
    const auto fields = kap::kpl::schema(plugin);
    KAP_ASSERT_EQ(fields.size(), static_cast<std::size_t>(2));
    KAP_ASSERT(fields[0].default_value.has_value());

    const auto [config, errors] = kap::kpl::build_config(plugin, {});
    KAP_ASSERT(errors.empty());
    KAP_ASSERT_EQ(config.at("retries").integer, static_cast<std::int64_t>(-1));
});

KAP_TEST("KPL schema names the members when an enum default is not one of them")
{
    const auto plugin           = kap::kpl::parse("schema { mode: enum { fast, slow } = quick }");
    const auto [config, errors] = kap::kpl::build_config(plugin, {});
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("'quick'") != std::string::npos);
    KAP_ASSERT(errors[0].find("fast, slow") != std::string::npos);
});

KAP_TEST("KPL rejects a duplicate record field")
{
    // Silently keeping one would drop the value the author was setting; in a
    // record step a dropped `cwd` runs the command in the wrong directory.
    KAP_ASSERT_THROWS(kap::diag::Error,
                      kap::kpl::parse("command t(project) { step { cmd: [\"x\"], cwd: \"a\", "
                                      "cwd: \"b\" } }"));
    KAP_ASSERT_THROWS(kap::diag::Error,
                      kap::kpl::parse("detect { file_contains { path: \"a\", path: \"b\" } }"));
});

KAP_TEST("KPL reports duplicate command names")
{
    const auto plugin =
        kap::kpl::parse("manifest { name = \"d\" version = \"1\" api_version = 1 }\n"
                        "command build(project) { step \"one\" }\n"
                        "command build(project) { step \"two\" }\n");
    const auto errors = kap::kpl::validate(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("duplicate command 'build'") != std::string::npos);
});

KAP_TEST("KPL rejects assignment to a name that was never declared")
{
    // `x = ...` updates; `let x = ...` declares. Without the distinction a
    // misspelled variable silently became a second variable nothing reads.
    const auto plugin = kap::kpl::parse("command t(project) { cmd = \"x\" step cmd }", "p.kpl");
    const auto errors = kap::kpl::type_check(plugin);
    KAP_ASSERT_EQ(errors.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(errors[0].find("undeclared name 'cmd'") != std::string::npos);

    const kap::kpl::Project project{};
    KAP_ASSERT_THROWS(kap::diag::Error, kap::kpl::evaluate(plugin, "t", project));

    // The design doc's accumulate idiom still works: declare, then update.
    const auto accumulating = kap::kpl::parse("command t(project, extra) {"
                                              "  let cmd = [\"cmake\"]"
                                              "  if true { cmd = cmd + [\"--build\"] }"
                                              "  cmd = cmd + extra"
                                              "  step cmd"
                                              "}");
    KAP_ASSERT(kap::kpl::type_check(accumulating).empty());
    const auto spec = kap::kpl::evaluate(accumulating, "t", project, {}, {"out"});
    KAP_ASSERT_EQ(spec.steps[0].command.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(spec.steps[0].command[2], std::string("out"));
});

KAP_TEST("KPL index errors point at the subscript")
{
    // Regression: the Index node was built with a default-constructed token,
    // so every out-of-range error reported line 1, column 1.
    const auto              plugin = kap::kpl::parse("command t(project) {\n"
                                                     "  let l = [\"a\"]\n"
                                                     "  step l[4]\n"
                                                     "}\n",
                                        "p.kpl");
    const kap::kpl::Project project{};
    try {
        kap::kpl::evaluate(plugin, "t", project);
        KAP_ASSERT(false);
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT_EQ(error.diagnostic().location.line, 3);
    }
});

// --- `else if` (Milestone 8 regression) ---------------------------------------------
//
// `else if` never parsed. The statement parser consumed the `if` with
// match_text and then called statement(), which arrived with the keyword
// already eaten — so the condition parsed as an expression statement and the
// block's `{` was read as a record literal. The reported error was "expected
// ':' after record field", pointing at the first line of the else body, which
// is about as far from the real cause as a parser error can get.
//
// It was found by writing a plugin that needed it, not by these tests, which
// is why they exist now.

KAP_TEST("else if parses and takes the second branch")
{
    const kap::kpl::Plugin plugin = kap::kpl::parse(R"(
manifest { name = "chain" version = "1.0.0" api_version = 1 }
schema { a: bool = false  b: bool = false }
command build(project, config, extra) {
  if config.a {
    step ["echo", "first"]
  } else if config.b {
    step ["echo", "second"]
  } else {
    step ["echo", "third"]
  }
}
)");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());

    kap::kpl::Project project;
    project.root = "/tmp";

    const auto run = [&](bool a, bool b) {
        std::map<std::string, kap::kpl::Value> config;
        config["a"] = kap::kpl::Value::boolean_value(a);
        config["b"] = kap::kpl::Value::boolean_value(b);
        return kap::kpl::evaluate(plugin, "build", project, config, {});
    };

    KAP_ASSERT_EQ(run(true, false).steps.front().command.back(), std::string("first"));
    KAP_ASSERT_EQ(run(false, true).steps.front().command.back(), std::string("second"));
    KAP_ASSERT_EQ(run(false, false).steps.front().command.back(), std::string("third"));
    // Both true: the first arm wins, as in every language with this shape.
    KAP_ASSERT_EQ(run(true, true).steps.front().command.back(), std::string("first"));
});

KAP_TEST("else if chains to any depth")
{
    const kap::kpl::Plugin plugin = kap::kpl::parse(R"(
manifest { name = "deep" version = "1.0.0" api_version = 1 }
schema { n: int = 0 }
command build(project, config, extra) {
  if config.n == 1 {
    step ["echo", "one"]
  } else if config.n == 2 {
    step ["echo", "two"]
  } else if config.n == 3 {
    step ["echo", "three"]
  } else if config.n == 4 {
    step ["echo", "four"]
  } else {
    step ["echo", "many"]
  }
}
)");
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());

    kap::kpl::Project project;
    project.root   = "/tmp";
    const auto run = [&](std::int64_t n) {
        std::map<std::string, kap::kpl::Value> config;
        config["n"] = kap::kpl::Value::integer_value(n);
        return kap::kpl::evaluate(plugin, "build", project, config, {})
            .steps.front()
            .command.back();
    };

    KAP_ASSERT_EQ(run(1), std::string("one"));
    KAP_ASSERT_EQ(run(3), std::string("three"));
    KAP_ASSERT_EQ(run(4), std::string("four"));
    KAP_ASSERT_EQ(run(9), std::string("many"));
});

KAP_TEST("an else-if with no trailing else simply falls through")
{
    const kap::kpl::Plugin plugin = kap::kpl::parse(R"(
manifest { name = "nofallback" version = "1.0.0" api_version = 1 }
schema { a: bool = false  b: bool = false }
command build(project, config, extra) {
  step ["always"]
  if config.a {
    step ["echo", "first"]
  } else if config.b {
    step ["echo", "second"]
  }
}
)");
    kap::kpl::Project      project;
    project.root = "/tmp";
    std::map<std::string, kap::kpl::Value> config;
    config["a"] = kap::kpl::Value::boolean_value(false);
    config["b"] = kap::kpl::Value::boolean_value(false);

    const kap::kpl::CommandSpec spec = kap::kpl::evaluate(plugin, "build", project, config, {});
    KAP_ASSERT_EQ(spec.steps.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(spec.steps.front().command.front(), std::string("always"));
});

// --- an enum member named `none` (Milestone 8) ----------------------------------------

KAP_TEST("a schema enum may not declare a member named none")
{
    // §5.5's `pattern` rule lists `none` as a literal, so `none => ...` in a
    // match arm reads as the absent-value literal rather than as the member.
    // The exhaustiveness checker then reports the member as uncovered, which is
    // true but reads like a bug in the checker. Refusing the declaration up
    // front is much kinder — and it is caught even when the plugin has no
    // match over the field yet, so it cannot lie in wait for whoever adds one.
    const kap::kpl::Plugin plugin = kap::kpl::parse(R"(
manifest { name = "nonenum" version = "1.0.0" api_version = 1 }
schema { checker: enum { none, mypy } = none }
command build(project, config, extra) { step ["echo"] }
)");
    const auto [values, errors]   = kap::kpl::build_config(plugin, {});
    KAP_ASSERT(!errors.empty());
    bool mentioned = false;
    for (const std::string& error : errors)
        mentioned = mentioned || error.find("cannot be matched") != std::string::npos;
    KAP_ASSERT(mentioned);
});

KAP_TEST("an enum whose members avoid none is accepted")
{
    const kap::kpl::Plugin plugin = kap::kpl::parse(R"(
manifest { name = "okenum" version = "1.0.0" api_version = 1 }
schema { checker: enum { off, mypy } = off }
command build(project, config, extra) {
  let tool = match config.checker {
    off  => "ruff",
    mypy => "mypy",
  }
  step ["uv", "run", tool]
}
)");
    const auto [values, errors]   = kap::kpl::build_config(plugin, {});
    KAP_ASSERT(errors.empty());
    KAP_ASSERT(kap::kpl::type_check(plugin).empty());
});

// --- `requires` extraction (Milestone 9) ---------------------------------------------
//
// `kap doctor` ships as a KPL plugin (§4), but KPL cannot see other plugins —
// so the core reads their `requires` blocks and injects the result. This is
// that reader.

KAP_TEST("requirements reads any_of and optional as bare words")
{
    const kap::kpl::Plugin       plugin = kap::kpl::parse(R"(
manifest { name = "cmake-cpp" version = "1.0.0" api_version = 1 }
requires {
  any_of   [cmake]
  optional [ninja, make, ccache]
}
)");
    const kap::kpl::Requirements needs  = kap::kpl::requirements(plugin);
    KAP_ASSERT_EQ(needs.required.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(needs.required.front(), std::string("cmake"));
    KAP_ASSERT_EQ(needs.optional.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(needs.optional[2], std::string("ccache"));
});

KAP_TEST("requirements accepts quoted names, which a dashed tool needs")
{
    // `golangci-lint` cannot be a bare identifier, so both spellings appear in
    // real plugins and both have to work.
    const kap::kpl::Plugin       plugin = kap::kpl::parse(R"(
manifest { name = "go" version = "1.0.0" api_version = 1 }
requires {
  any_of   [go]
  optional [gofumpt, "golangci-lint"]
}
)");
    const kap::kpl::Requirements needs  = kap::kpl::requirements(plugin);
    KAP_ASSERT_EQ(needs.optional.size(), static_cast<std::size_t>(2));
    KAP_ASSERT_EQ(needs.optional[1], std::string("golangci-lint"));
});

KAP_TEST("requirements keeps every member of an any_of group")
{
    // The grouping is what makes `any_of [ss, lsof, netstat]` mean "one of
    // these" rather than "all three". Losing it here would make `kap doctor`
    // report a healthy machine as missing two tools.
    const kap::kpl::Plugin       plugin = kap::kpl::parse(R"(
manifest { name = "ports" version = "1.0.0" api_version = 1 }
requires {
  any_of   [ss, lsof, netstat]
  optional []
}
)");
    const kap::kpl::Requirements needs  = kap::kpl::requirements(plugin);
    KAP_ASSERT_EQ(needs.required.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(needs.required[0], std::string("ss"));
    KAP_ASSERT_EQ(needs.required[2], std::string("netstat"));
    KAP_ASSERT(needs.optional.empty());
});

KAP_TEST("a plugin with no requires block requires nothing")
{
    const kap::kpl::Plugin       plugin = kap::kpl::parse(R"(
manifest { name = "bare" version = "1.0.0" api_version = 1 }
)");
    const kap::kpl::Requirements needs  = kap::kpl::requirements(plugin);
    KAP_ASSERT(needs.required.empty());
    KAP_ASSERT(needs.optional.empty());
});

KAP_TEST("an unrecognised requires directive is ignored, not fatal")
{
    // The `requires` block is a declaration surface KPL may grow later; a
    // directive this kap does not know is not a reason to refuse the plugin,
    // and the api_version gate is what guards against real incompatibility.
    const kap::kpl::Plugin       plugin = kap::kpl::parse(R"(
manifest { name = "future" version = "1.0.0" api_version = 1 }
requires {
  any_of         [make]
  recommended    [ccache]
}
)");
    const kap::kpl::Requirements needs  = kap::kpl::requirements(plugin);
    KAP_ASSERT_EQ(needs.required.size(), static_cast<std::size_t>(1));
    KAP_ASSERT(needs.optional.empty());
});
