// core/toml.cpp
//
// Implementation of the minimal TOML-subset parser declared in core/toml.hpp.
// The grammar is small on purpose (design doc §9): tables, bare keys, and the
// scalar values kap's config schema actually uses. Anything else produces a
// located diagnostic — fail fast, built from stdlib only.

#include "core/toml.hpp"

#include <algorithm>

#include <charconv>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "core/diag.hpp"

namespace kap
{
namespace toml
{

// --- Value constructors ---------------------------------------------------------
// Every member is assigned explicitly (rather than using a designated-
// initialiser aggregate) so no -Wmissing-field-initializers warning can fire.

Value make_string(std::string s)
{
    Value v;
    v.kind = Value::Kind::String;
    v.str  = std::move(s);
    return v;
}

Value make_integer(std::int64_t i)
{
    Value v;
    v.kind    = Value::Kind::Integer;
    v.integer = i;
    return v;
}

Value make_boolean(bool b)
{
    Value v;
    v.kind    = Value::Kind::Boolean;
    v.boolean = b;
    return v;
}

Value make_array(std::vector<Value> items)
{
    Value v;
    v.kind  = Value::Kind::Array;
    v.array = std::move(items);
    return v;
}

Value make_table()
{
    Value v;
    v.kind = Value::Kind::Table;
    return v;
}

bool is_table(const Value& v)
{
    return v.kind == Value::Kind::Table;
}

bool is_array(const Value& v)
{
    return v.kind == Value::Kind::Array;
}

bool is_string(const Value& v)
{
    return v.kind == Value::Kind::String;
}

std::optional<Value> Document::get(std::string_view path) const
{
    // Walk the dotted path through nested tables, failing on any missing
    // segment or any non-table value encountered on the way.
    const Value* current = &root_;
    std::size_t  start   = 0;
    for (;;) {
        const std::size_t dot = path.find('.', start);
        const std::size_t end = dot == std::string_view::npos ? path.size() : dot;
        if (end == start || current->kind != Value::Kind::Table) {
            return std::nullopt; // empty segment or stepped through a scalar
        }
        const std::string_view segment = path.substr(start, end - start);
        const auto             it      = current->table.find(std::string(segment));
        if (it == current->table.end()) {
            return std::nullopt;
        }
        current = &it->second;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return *current;
}

namespace
{

// A cursor over the input that remembers its 1-based line/column so every
// failure can be reported with a precise location.
class Parser
{
public:
    Parser(std::string_view text, std::string source_name)
        : text_(text), source_name_(std::move(source_name))
    {}

    Document run()
    {
        Document doc;
        parse_body(doc.root());
        return doc;
    }

private:
    static constexpr char kEof = '\0';

    bool at_end() const
    {
        return pos_ >= text_.size();
    }

    char peek(std::size_t ahead = 0) const
    {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : kEof;
    }

    // Consume one character, tracking the cursor's line and column.
    void advance()
    {
        if (at_end()) {
            return;
        }
        if (text_[pos_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++pos_;
    }

    [[noreturn]] void fail(const std::string& message) const
    {
        throw diag::Error{diag::error(
            message,
            diag::Location{source_name_, static_cast<int>(line_), static_cast<int>(col_)})};
    }

    // Skip the current line (its comment part) and reposition at the newline.
    void skip_inline_comment()
    {
        if (peek() == '#') {
            while (!at_end() && peek() != '\n') {
                advance();
            }
        }
    }

    // Skip spaces and tabs (never newlines). Carriage returns are treated as
    // whitespace too, so a file saved on Windows (CRLF line endings) parses
    // identically to one saved on Linux — otherwise every single line would
    // die on "expected end of line" at the stray '\r'.
    void skip_inline_ws()
    {
        while (peek() == ' ' || peek() == '\t' || peek() == '\r') {
            advance();
        }
    }

    // Skip blanks and whole comment lines between statements.
    void skip_blank_lines_and_comments()
    {
        for (;;) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r') {
                advance();
            }
            if (peek() == '#') {
                while (!at_end() && peek() != '\n') {
                    advance();
                }
                continue;
            }
            return;
        }
    }

    // After a value/header has been consumed, only whitespace and an optional
    // trailing comment may remain on the line.
    void require_end_of_line()
    {
        skip_inline_ws();
        skip_inline_comment();
        if (at_end() || peek() == '\n') {
            return;
        }
        // A '.' or an exponent right after an integer is a float, which this
        // subset does not implement. Saying so beats "expected end of line",
        // which reads as a syntax error in code that is valid TOML.
        if (peek() == '.' || peek() == 'e' || peek() == 'E' || peek() == '-' || peek() == ':') {
            fail("floating-point numbers and datetimes are not supported");
        }
        fail("expected end of line");
    }

    // Read a bare key: [A-Za-z0-9_-]+ (underscore/dash keep TOML keys like
    // "build_dir" and "cmake-cpp" parseable).
    std::string read_bare_key()
    {
        // `[[name]]` is an array of tables. The second '[' would otherwise be
        // reported as "expected a key name", which describes the symptom
        // rather than the cause.
        if (peek() == '[') {
            fail("arrays of tables ([[name]]) are not supported");
        }
        const std::size_t start = pos_;
        while (is_bare_key_char(peek())) {
            advance();
        }
        if (pos_ == start) {
            fail("expected a key name");
        }
        return std::string(text_.substr(start, pos_ - start));
    }

    static bool is_bare_key_char(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    }

    static bool is_digit(char c)
    {
        return c >= '0' && c <= '9';
    }

    // parse_body: the statement loop of the document.
    //
    // `current` is the table that plain `key = value` lines land in. It starts
    // at the document root and is *re-pointed* by every `[table.header]` line —
    // that retargeting is the whole point of a section header. (An earlier
    // version created the table and then dropped the result on the floor, so
    // `[server]` / `host = "..."` produced a root-level `host` instead of
    // `server.host`. Everything downstream — config lookup, plugin overrides —
    // silently read the wrong keys.)
    //
    // Holding a `Value*` across later insertions is safe because Value::table
    // is a std::map, and std::map guarantees that references and pointers to
    // existing elements survive any insertion.
    void parse_body(Value& root)
    {
        Value* current = &root;

        for (;;) {
            skip_blank_lines_and_comments();
            if (at_end()) {
                return;
            }

            if (peek() == '[') {
                advance(); // consume '['
                skip_inline_ws();
                const std::string path = read_table_path();
                skip_inline_ws();
                if (peek() != ']') {
                    fail("expected ']' to close the table header");
                }
                advance(); // consume ']'
                require_end_of_line();

                // TOML forbids opening the same table twice; without this a
                // duplicated `[server]` would silently merge two blocks (and
                // the "duplicate key" check below would fire on a confusing
                // line instead of the real culprit).
                if (!defined_headers_.insert(path).second) {
                    fail("table '" + path + "' is defined more than once");
                }

                // Table headers are always resolved from the document root,
                // never relative to the previous header.
                current = &descend(root, path);
                continue;
            }

            parse_key_value(*current);
        }
    }

    // A dotted table path is a sequence of bare keys joined by '.'. The empty
    // path ("[]") is rejected by read_bare_key's "expected a key name".
    std::string read_table_path()
    {
        std::string path;
        for (;;) {
            const std::string segment = read_bare_key();
            if (!path.empty()) {
                path += '.';
            }
            path += segment;
            skip_inline_ws();
            if (peek() != '.') {
                return path;
            }
            advance(); // consume '.'
            skip_inline_ws();
        }
    }

    // Ensure `root.path` exists as a (possibly nested) table, creating missing
    // levels, and return a reference to the innermost one. Stepping through an
    // existing non-table is an error — that is where "A is not a table" style
    // config bugs show up.
    Value& descend(Value& root, const std::string& path)
    {
        Value*      current = &root;
        std::size_t start   = 0;
        for (;;) {
            const std::size_t dot     = path.find('.', start);
            const std::size_t end     = dot == std::string::npos ? path.size() : dot;
            const std::string segment = path.substr(start, end - start);

            if (current->kind != Value::Kind::Table) {
                fail("cannot create table '" + path +
                     "': an earlier value at this path is not a table");
            }
            const auto inserted = current->table.emplace(segment, make_table());
            if (!inserted.second && inserted.first->second.kind != Value::Kind::Table) {
                fail("cannot create table '" + path +
                     "': an earlier value at this path is not a table");
            }
            current = &inserted.first->second;

            if (dot == std::string::npos) {
                return *current;
            }
            start = dot + 1;
        }
    }

    // parse_key_value:    key = value   (bare keys only for v1)
    void parse_key_value(Value& table)
    {
        const std::string key = read_bare_key();
        skip_inline_ws();
        if (peek() != '=') {
            fail("expected '=' after key '" + key + "'");
        }
        advance(); // consume '='
        skip_inline_ws();

        Value value = parse_value();
        require_end_of_line();

        if (table.kind != Value::Kind::Table) {
            fail("cannot set key '" + key + "': an earlier value at this path is not a table");
        }
        // TOML forbids redefining a key in the same table.
        const auto inserted = table.table.emplace(key, std::move(value));
        if (!inserted.second) {
            fail("duplicate key '" + key + "'");
        }
    }

    Value parse_value()
    {
        const char c = peek();
        if (c == '"') {
            return parse_string();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == 't' || c == 'f') {
            return parse_boolean();
        }
        if (c == '-' || c == '+' || is_digit(c)) {
            return parse_integer();
        }
        // Name the TOML features this subset deliberately omits (see the
        // header, and the Milestone 1 notes in the design doc). "expected a
        // value" is technically accurate for all of them and helps with none:
        // someone who wrote a perfectly ordinary inline table deserves to be
        // told that *this parser* does not do inline tables, not to go hunting
        // for a typo that is not there.
        if (c == '\'') {
            fail("literal 'single-quoted' strings are not supported; use a "
                 "\"double-quoted\" string");
        }
        if (c == '{') {
            fail("inline tables are not supported; use a [table] header instead");
        }
        fail("expected a value");
    }

    Value parse_string()
    {
        advance(); // consume opening '"'
        std::string out;
        for (;;) {
            if (at_end() || peek() == '\n') {
                fail("unterminated string literal");
            }
            const char c = peek();

            if (c == '"') {
                advance(); // consume closing '"'
                return make_string(std::move(out));
            }

            if (c != '\\') {
                out += c;
                advance();
                continue;
            }

            // Escape sequences; anything unknown is an error so a typo like
            // "\q" cannot silently change meaning.
            advance(); // consume '\'
            if (at_end() || peek() == '\n') {
                fail("unterminated string literal");
            }
            const char esc = peek();
            switch (esc) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'r':
                    out += '\r';
                    break;
                default:
                    fail(std::string("unsupported escape sequence '\\") + esc + "'");
            }
            advance(); // consume the escaped character
        }
    }

    Value parse_integer()
    {
        // from_chars handles the sign and rejects malformed/overflowing text
        // with a located error, so a bad number is caught here rather than
        // buried in config merge later.
        const std::size_t start = pos_;
        if (peek() == '-' || peek() == '+') {
            advance();
        }
        if (!is_digit(peek())) {
            fail("expected digits in integer");
        }
        // TOML forbids leading zeros ("07" is not 7), because they read as
        // octal to most humans. Reject them instead of guessing.
        if (peek() == '0' && is_digit(peek(1))) {
            fail("integers may not have leading zeros");
        }
        while (is_digit(peek())) {
            advance();
        }
        const std::string_view digits = text_.substr(start, pos_ - start);

        std::int64_t parsed = 0;
        // std::from_chars does not accept a leading '+', so skip past it.
        const char* first  = digits.data() + (digits.front() == '+' ? 1 : 0);
        const char* last   = digits.data() + digits.size();
        const auto  result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc{} || result.ptr != last) {
            fail("integer out of range: '" + std::string(digits) + "'");
        }
        return make_integer(parsed);
    }

    Value parse_boolean()
    {
        const std::size_t start = pos_;
        while ((peek() >= 'a' && peek() <= 'z') || (peek() >= 'A' && peek() <= 'Z')) {
            advance();
        }
        const std::string_view word = text_.substr(start, pos_ - start);
        if (word == "true") {
            return make_boolean(true);
        }
        if (word == "false") {
            return make_boolean(false);
        }
        fail("expected 'true' or 'false'");
    }

    // Inside an array, newlines are insignificant: TOML lets a long list be
    // wrapped across lines, which config files in the wild rely on. Comments
    // may appear on those continuation lines too.
    void skip_array_ws()
    {
        for (;;) {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') {
                advance();
            }
            if (peek() == '#') {
                while (!at_end() && peek() != '\n') {
                    advance();
                }
                continue;
            }
            return;
        }
    }

    Value parse_array()
    {
        advance(); // consume '['
        Value array = make_array();

        for (;;) {
            skip_array_ws();
            if (peek() == ']') {
                // Either an empty array, or a trailing comma before the
                // close bracket — both are legal TOML.
                break;
            }
            if (at_end()) {
                fail("expected ']' to close the array");
            }

            const char c = peek();
            // Only scalar element types are allowed (design doc: config arrays
            // are list<str>/list<int>); nested arrays or tables are rejected.
            if (c == '"' || is_digit(c) || c == '-' || c == '+' || c == 't' || c == 'f') {
                array.array.push_back(parse_value());
            } else {
                fail("array elements must be strings, integers, or booleans");
            }

            skip_array_ws();
            if (peek() != ',') {
                break;
            }
            advance(); // consume ',' and look for another element
        }

        skip_array_ws();
        if (peek() != ']') {
            fail("expected ']' to close the array");
        }
        advance(); // consume ']'
        return array;
    }

    std::string_view text_;
    std::string      source_name_;
    std::size_t      pos_  = 0;
    std::size_t      line_ = 1;
    std::size_t      col_  = 1;

    // Every `[header]` seen so far, so a duplicate can be reported.
    std::set<std::string> defined_headers_;
};

} // namespace

Document parse(std::string_view text, std::string source_name)
{
    Parser parser(text, std::move(source_name));
    return parser.run();
}

// --- writing ----------------------------------------------------------------------

std::string escape_string(std::string_view text)
{
    // The inverse of the parser's escape handling, and deliberately no wider:
    // this subset understands \" \\ \n \t, so those are the four that get
    // an escape. Any other control character is written as-is rather than as a
    // \uXXXX the parser would then reject — writing something we cannot read
    // back would be worse than the (already unusual) raw byte.
    std::string out = "\"";
    for (const char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    out += '"';
    return out;
}

namespace
{

// A bare key needs no quoting; anything else does. The parser accepts dashes
// in bare keys (plugin names like `cmake-cpp` are the reason), so the writer
// must too, or a round-trip would gratuitously quote every plugin section.
bool is_bare_key(std::string_view key)
{
    if (key.empty())
        return false;
    for (const char ch : key) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok)
            return false;
    }
    return true;
}

std::string render_key(const std::string& key)
{
    return is_bare_key(key) ? key : escape_string(key);
}

// One scalar or array, as it appears on the right of an `=`.
std::string render_scalar(const Value& value)
{
    switch (value.kind) {
        case Value::Kind::String:
            return escape_string(value.str);
        case Value::Kind::Integer:
            return std::to_string(value.integer);
        case Value::Kind::Boolean:
            return value.boolean ? "true" : "false";
        case Value::Kind::Array:
            {
                std::string out = "[";
                for (std::size_t index = 0; index < value.array.size(); ++index) {
                    if (index != 0)
                        out += ", ";
                    out += render_scalar(value.array[index]);
                }
                out += "]";
                return out;
            }
        case Value::Kind::Table:
            // Unreachable through write(), which routes tables to headers. An
            // inline table would be the natural rendering, but the parser
            // rejects those (see core/toml.hpp), so emitting one would produce
            // a file kap itself cannot read.
            return "{}";
    }
    return "\"\"";
}

// Emit `table`, then recurse into its sub-tables. `prefix` is the dotted
// header path built so far.
void write_table(const Value& table, const std::string& prefix, std::string& out)
{
    // Scalars first: TOML binds every `key = value` to the most recent header,
    // so a scalar written after a sub-table header would silently land in the
    // wrong table. This ordering is a correctness requirement, not a style.
    for (const auto& [key, value] : table.table) {
        if (value.kind == Value::Kind::Table)
            continue;
        out += render_key(key) + " = " + render_scalar(value) + "\n";
    }

    for (const auto& [key, value] : table.table) {
        if (value.kind != Value::Kind::Table)
            continue;
        const std::string header =
            prefix.empty() ? render_key(key) : prefix + "." + render_key(key);

        // A table holding nothing but sub-tables does not need a header of its
        // own: writing `[plugins.cmake-cpp]` already brings `plugins` into
        // existence. Emitting it anyway leaves a bare `[plugins]` floating
        // above the entries, which reads like a mistake to anyone skimming a
        // lockfile. A genuinely empty table is the exception — there the header
        // is the only thing recording that it exists at all.
        const bool has_scalars =
            std::any_of(value.table.begin(), value.table.end(), [](const auto& entry) {
                return entry.second.kind != Value::Kind::Table;
            });
        if (has_scalars || value.table.empty()) {
            if (!out.empty() && out.back() != '\n')
                out += "\n";
            out += "\n[" + header + "]\n";
        }
        write_table(value, header, out);
    }
}

} // namespace

std::string write(const Value& table)
{
    if (table.kind != Value::Kind::Table)
        return {};
    std::string out;
    write_table(table, {}, out);

    // A leading blank line happens when the root table has no scalars of its
    // own and goes straight into a `[header]`. Harmless, but a file that opens
    // with an empty line looks like a mistake.
    while (!out.empty() && out.front() == '\n')
        out.erase(0, 1);
    return out;
}

} // namespace toml
} // namespace kap
