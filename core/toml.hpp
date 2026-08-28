#pragma once

// core/toml.hpp
//
// A minimal TOML-subset parser (design doc Milestone 1 §9: "Minimal TOML
// subset parser (in-tree)"). It intentionally understands only what kap needs
// — user config (`~/.config/kap/config.toml`, `./kap.toml`), the installed-
// plugins lockfile, and the registry index:
//
//   [section]                  tables (dotted paths: plugins.cmake-cpp)
//   key = "basic string"       strings with \" \\ \n \t escapes
//   key = 42                   integers (64-bit, decimal)
//   key = true / false         booleans
//   key = [1, 2, 3]            arrays of scalars (no nested arrays/tables)
//   # comment                  comments to end of line
//
// Strings are stored in a single variant-like Value so the config-merging code
// (Milestone 6) and the registry tooling can share one representation.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kap
{
namespace toml
{

// A parsed value. Only the kinds above exist; everything else is rejected at
// parse time with a located diagnostic, which is exactly the fail-fast
// behaviour design doc §5.7 asks for ("typos fail fast with a clear error").
struct Value
{
    enum class Kind
    {
        String,
        Integer,
        Boolean,
        Array,
        Table,
    };

    Kind kind = Kind::Table;

    std::string str;
    std::int64_t integer = 0;
    bool boolean = false;

    std::vector<Value> array;
    std::map<std::string, Value> table;   // std::map ⇒ deterministic iteration
};

// --- Value constructors ---------------------------------------------------------
// Aggregate initialisation with designated initialisers (`Value{.kind = ...,
// .str = ...}`) leaves the remaining members unmentioned, which makes GCC's
// -Wmissing-field-initializers fire and — because CI builds with -Werror —
// breaks the build. These factories set every member explicitly, so callers
// get one readable expression and zero warnings.
Value make_string(std::string s);
Value make_integer(std::int64_t i);
Value make_boolean(bool b);
Value make_array(std::vector<Value> items = {});
Value make_table();

// helper accessors
bool is_table(const Value& v);
bool is_array(const Value& v);
bool is_string(const Value& v);

// A parsed TOML document: one top-level table plus the ability to look
// anything up by dotted path.
class Document
{
  public:
    // Look up a dotted path such as "plugins.cmake-cpp.generator". Returns
    // std::nullopt when any component is missing or the path steps through a
    // non-table value.
    std::optional<Value> get(std::string_view path) const;

    const Value& root() const { return root_; }
    Value& root() { return root_; }

  private:
    Value root_;
};

// Parse `text` as TOML. `source_name` is only used to decorate error
// messages (e.g. "kap.toml"); it may be empty. Throws diag::Error on any
// syntax problem, with the offending line and column attached.
Document parse(std::string_view text, std::string source_name = {});

} // namespace toml
} // namespace kap