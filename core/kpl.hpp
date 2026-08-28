#pragma once

// core/kpl.hpp
//
// The KPL front-end. This first layer turns plugin source into a located token
// stream; the parser in kpl.cpp consumes the same public token types.

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"

namespace kap::kpl
{

enum class TokenKind
{
    Identifier,
    String,
    Integer,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    LeftParen,
    RightParen,
    Colon,
    Comma,
    Dot,
    Plus,
    Minus,
    Star,
    Slash,
    Bang,
    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr,
    Arrow,
    End,
};

struct Token
{
    TokenKind    kind = TokenKind::End;
    std::string  text;
    std::int64_t integer = 0;
    int          line    = 1;
    int          column  = 1;
};

std::vector<Token> lex(std::string_view source, std::string source_name = {});

struct Expr
{
    enum class Kind
    {
        String,
        Integer,
        Boolean,
        None,
        Name,
        List,
        Record,
        Unary,
        Binary,
        Call,
        Member,
        Index,
        Conditional,
        // children = [subject, pattern0, result0, pattern1, result1, ...].
        // A Name pattern is a bare word (an enum member), not a variable read.
        Match
    };
    Kind                     kind = Kind::None;
    Token                    token;
    std::vector<Expr>        children;
    std::vector<std::string> names;
};

struct Statement
{
    enum class Kind
    {
        Assignment,
        Let,
        Expression,
        Step,
        If,
        For,
        Match,
        Concurrent,
        ReportFreedSpace,
        // A declaration inside `detect` or `requires`: a directive name plus
        // its arguments, e.g. `file_exists "CMakeLists.txt"` or
        // `optional [ninja, make]`. `name` holds the directive; `expressions`
        // holds the arguments.
        Directive
    };
    Kind                     kind = Kind::Expression;
    Token                    token;
    std::string              name;
    std::string              type_name;
    std::vector<std::string> names;
    std::vector<Expr>        expressions;
    std::vector<Statement>   body;
    std::vector<Statement>   otherwise;
};

struct Block
{
    std::vector<Statement> statements;
};

struct Command
{
    std::string              name;
    std::vector<std::string> parameters;
    Block                    body;
    Token                    token;
};

struct Plugin
{
    // Where this plugin was parsed from ("<name>/plugin.kpl", or empty for an
    // in-memory source). Carried on the AST rather than passed separately so
    // that run-time diagnostics — raised long after parsing, from evaluate() —
    // can still name the file the plugin author has to open.
    std::string source_name;

    std::optional<Block> manifest;
    std::optional<Block> detect;
    std::optional<Block> requires_block;
    std::optional<Block> schema;
    std::vector<Command> commands;
};

struct Value
{
    enum class Kind
    {
        None,
        String,
        Integer,
        Boolean,
        List,
        Record
    };

    Kind                         kind = Kind::None;
    std::string                  string;
    std::int64_t                 integer = 0;
    bool                         boolean = false;
    std::vector<Value>           list;
    std::map<std::string, Value> record;

    static Value none();
    static Value string_value(std::string value);
    static Value integer_value(std::int64_t value);
    static Value boolean_value(bool value);
    static Value list_value(std::vector<Value> value);
    static Value record_value(std::map<std::string, Value> value);
};

// The read-only host object handed to every command block (design doc §3.4).
//
// Everything KPL can learn about the outside world arrives through these
// callbacks, and nothing else: there is no raw filesystem API, no process
// spawning, and no unfiltered environment access in the language. Making them
// std::function rather than direct calls into core/fs.hpp is what lets the
// unit tests drive the interpreter against a mocked project with no disk at
// all (design doc Milestone 3), while production code uses host_project()
// below.
//
// A callback left empty means "this capability is unavailable here". Queries
// degrade to a safe answer (false / none / an empty list); `read` — where an
// empty string would be indistinguishable from an empty file — raises instead.
struct Project
{
    std::string              root;
    std::vector<std::string> matched_files;

    std::function<bool(std::string_view)>                       exists; // path -> bool
    std::function<bool(std::string_view)>                       tool;   // PATH lookup
    std::function<std::string(std::string_view)>                read;   // capped file read
    std::function<std::vector<std::string>(std::string_view)>   glob;   // capped glob
    std::function<std::optional<std::string>(std::string_view)> env;    // filtered env
};

// Build the production host object: reads sandboxed to `root`, a PATH scan for
// `tool`, and a deny-listed environment lookup (design doc §7). `root` should
// be an existing directory; a path that escapes it is refused, not clamped.
Project host_project(std::string root, std::vector<std::string> matched_files = {});

struct Step
{
    std::vector<std::string>           command;
    std::optional<std::string>         cwd;
    std::map<std::string, std::string> environment;
    std::optional<std::string>         label;
};

struct CommandSpec
{
    std::vector<Step> steps;
    bool              concurrent         = false;
    bool              report_freed_space = false;
};

struct SchemaField
{
    std::string              name;
    std::string              type;
    std::vector<std::string> enum_values;
    std::optional<Value>     default_value;
};

// Read the schema declarations and merge overrides on top of their defaults.
// Errors are returned instead of thrown so callers can report every bad key.
std::vector<SchemaField> schema(const Plugin& plugin);
std::pair<std::map<std::string, Value>, std::vector<std::string>>
build_config(const Plugin& plugin, const std::map<std::string, Value>& overrides);

// Check command expressions without executing them. Unknown values from the
// host (notably config fields) are allowed where their runtime type is not
// statically knowable.
std::vector<std::string> type_check(const Plugin& plugin);

// Evaluate one command without spawning a process. `config` and `extra` are
// supplied by the host; project callbacks are the only filesystem/tool access
// available to KPL.
CommandSpec evaluate(const Plugin&                       plugin,
                     std::string_view                    command_name,
                     const Project&                      project,
                     const std::map<std::string, Value>& config = {},
                     const std::vector<std::string>&     extra  = {});

// The CommandSpec <-> JSON contract from design doc §5.4. This is the shape a
// plugin's `tests/expected/*.steps.json` golden file is written in, and the
// shape `--dry-run` will render from in Milestone 5.
//
// to_json always writes every field, including the null/empty defaults, so two
// specs that behave identically serialize identically and can be compared as
// text. spec_from_json is the inverse and treats the optional fields as
// optional, so a golden file only has to spell out what it cares about.
json::Value to_json(const CommandSpec& spec);
CommandSpec spec_from_json(const json::Value& value, const std::string& source_name = {});

// Validate the manifest contract used by the plugin loader. The parser keeps
// blocks generic so later KPL features can evolve independently; this check
// is the narrow compatibility gate needed by `kap plugin doctor`.
std::vector<std::string> validate(const Plugin& plugin, int supported_api_version = 1);

// Parse one plugin into an AST. Keywords are represented by identifier tokens
// and validated here, keeping lexical evolution separate from the grammar.
Plugin parse(std::string_view source, std::string source_name = {});

} // namespace kap::kpl
