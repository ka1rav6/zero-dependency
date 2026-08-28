// tests/test_json.cpp
//
// Unit tests for the minimal JSON subset (core/json.hpp). The parser backs
// `kap plugin test`'s golden files (design doc §5.2) and, from Milestone 4,
// the detection cache, so the properties that matter most here are
// round-tripping and deterministic output.

#include "core/diag.hpp"
#include "core/json.hpp"
#include "harness.hpp"

#include <cstddef>
#include <string>

KAP_TEST("JSON parses every supported scalar")
{
    const auto value =
        kap::json::parse(R"({"s": "text", "i": -42, "t": true, "f": false, "n": null})", "t.json");

    KAP_ASSERT_EQ(static_cast<int>(value.kind), static_cast<int>(kap::json::Value::Kind::Object));
    KAP_ASSERT_EQ(value.find("s")->string, std::string("text"));
    KAP_ASSERT_EQ(value.find("i")->integer, static_cast<std::int64_t>(-42));
    KAP_ASSERT(value.find("t")->boolean);
    KAP_ASSERT(!value.find("f")->boolean);
    KAP_ASSERT_EQ(static_cast<int>(value.find("n")->kind),
                  static_cast<int>(kap::json::Value::Kind::Null));
    KAP_ASSERT(value.find("missing") == nullptr);
});

KAP_TEST("JSON parses nested arrays and objects")
{
    const auto  value = kap::json::parse(R"({"steps":[{"cmd":["cmake","--build","build"]}]})");
    const auto* steps = value.find("steps");

    KAP_ASSERT_EQ(steps->array.size(), static_cast<std::size_t>(1));
    KAP_ASSERT_EQ(steps->array[0].find("cmd")->array.size(), static_cast<std::size_t>(3));
    KAP_ASSERT_EQ(steps->array[0].find("cmd")->array[2].string, std::string("build"));
});

KAP_TEST("JSON parses empty containers and tolerates whitespace")
{
    const auto value = kap::json::parse("  {\n \"a\" : [ ] ,\n \"b\" : { }\n }  \n");
    KAP_ASSERT(value.find("a")->array.empty());
    KAP_ASSERT(value.find("b")->object.empty());
});

KAP_TEST("JSON decodes the supported string escapes")
{
    const auto value = kap::json::parse(R"({"k":"a\"b\\c\nd\te\/f\r\bg"})");
    KAP_ASSERT_EQ(value.find("k")->string, std::string("a\"b\\c\nd\te/f\r\bg"));
});

KAP_TEST("JSON rejects malformed input with a location")
{
    // Every one of these is a distinct failure path, and each must point at
    // the offending character rather than at the end of the file.
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{\"a\": }", "t.json"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{\"a\" 1}", "t.json"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{a: 1}", "t.json"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("[1, 2", "t.json"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("\"unterminated", "t.json"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{} trailing", "t.json"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{\"a\":1,\"a\":2}", "t.json"));

    try {
        kap::json::parse("{\n  \"a\": tru\n}", "t.json");
        KAP_ASSERT(false);
    }
    catch (const kap::diag::Error& error) {
        KAP_ASSERT_EQ(error.diagnostic().location.line, 2);
        KAP_ASSERT(error.report().find("t.json:2:") != std::string::npos);
    }
});

KAP_TEST("JSON rejects the deliberately unsupported constructs")
{
    // Rejected, never misparsed — so adding either later is additive and
    // cannot change the meaning of a file that parses today.
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{\"a\": 1.5}"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{\"a\": 1e3}"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{\"a\": \"\\u0041\"}"));
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse("{\"a\": \"line\nbreak\"}"));
});

KAP_TEST("JSON rejects pathological nesting instead of overflowing the stack")
{
    KAP_ASSERT_THROWS(kap::diag::Error, kap::json::parse(std::string(4096, '[')));
});

KAP_TEST("JSON writes deterministic, sorted, re-parseable output")
{
    const auto        original = kap::json::parse(R"({"z":1,"a":[true,null,"x"],"m":{"k":"v"}})");
    const std::string compact  = kap::json::write(original, false);

    // Keys come out sorted regardless of input order, which is what makes a
    // golden file diffable.
    KAP_ASSERT_EQ(compact, std::string(R"({"a":[true,null,"x"],"m":{"k":"v"},"z":1})"));

    // Round trip: writing then re-parsing yields the same text again.
    KAP_ASSERT_EQ(kap::json::write(kap::json::parse(compact), false), compact);
});

KAP_TEST("JSON pretty output indents and ends with a newline")
{
    const auto text = kap::json::write(
        kap::json::make_object({{"cmd", kap::json::make_array({kap::json::make_string("ls")})}}));
    KAP_ASSERT_EQ(text, std::string("{\n  \"cmd\": [\n    \"ls\"\n  ]\n}\n"));
    // And it parses back to the same value.
    KAP_ASSERT_EQ(kap::json::parse(text).find("cmd")->array[0].string, std::string("ls"));
});

KAP_TEST("JSON escapes control characters it writes")
{
    const auto text = kap::json::write(kap::json::make_string("a\"b\\c\nd\te"), false);
    KAP_ASSERT_EQ(text, std::string(R"("a\"b\\c\nd\te")"));
    KAP_ASSERT_EQ(kap::json::parse(text).string, std::string("a\"b\\c\nd\te"));
});
