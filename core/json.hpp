#pragma once

// core/json.hpp
//
// A minimal JSON subset, in-tree (design doc §9: no nlohmann/json, no
// third-party anything). Two places in the design need JSON and neither needs
// all of it:
//
//   - the CommandSpec contract (§5.4), which `kap plugin test` compares
//     against a plugin's `tests/expected/*.steps.json`;
//   - the detection cache `.kap/cache.json` (§3.2 step 5).
//
// What is supported: objects, arrays, strings, 64-bit integers, true/false,
// null. What is not, and why:
//
//   - Floating point. Nothing in the design has a fractional value, and a
//     float round-trip would make cache keys and golden files depend on
//     printf precision. Rejected with a located diagnostic rather than
//     silently truncated.
//   - \uXXXX escapes. They would need UTF-16 surrogate pairing to be correct,
//     and every string kap writes is a path or an argv word.
//
// Both are *rejected*, never misparsed, so adding either later is additive and
// cannot change the meaning of a file that parses today. This is the same
// stance core/toml.hpp takes on the parts of TOML it omits.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kap
{
namespace json
{

// A parsed JSON value. std::map for objects, so iteration order is the key
// order — which makes the writer's output deterministic and therefore
// diffable, the property golden files live or die by.
struct Value
{
    enum class Kind
    {
        Null,
        Boolean,
        Integer,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;

    bool         boolean = false;
    std::int64_t integer = 0;
    std::string  string;

    std::vector<Value>           array;
    std::map<std::string, Value> object;

    // Look one key up in an object. Returns nullptr for a missing key or a
    // non-object, so callers can treat "absent" and "wrong shape" alike when
    // that is what they want.
    const Value* find(std::string_view key) const;
};

// Factories. Same rationale as core/toml.hpp's: designated initialisers would
// leave members unmentioned and trip -Wmissing-field-initializers under CI's
// -Werror, so every member is set explicitly in one place.
Value make_null();
Value make_boolean(bool value);
Value make_integer(std::int64_t value);
Value make_string(std::string value);
Value make_array(std::vector<Value> items = {});
Value make_object(std::map<std::string, Value> fields = {});

// Parse `text`. `source_name` decorates diagnostics (e.g. "build.steps.json")
// and may be empty. Throws diag::Error with a line and column on any syntax
// problem, including the deliberately unsupported constructs above.
Value parse(std::string_view text, std::string source_name = {});

// Render `value`. `pretty` emits two-space indentation and newlines (what
// golden files should be committed as); otherwise everything is on one line.
// Object keys are written in sorted order either way, so the same value always
// produces byte-identical output.
std::string write(const Value& value, bool pretty = true);

} // namespace json
} // namespace kap
