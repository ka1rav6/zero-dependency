#pragma once

// core/kpl.hpp
//
// The KPL front-end. This first layer turns plugin source into a located token
// stream; the parser in kpl.cpp consumes the same public token types.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
        Conditional
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
        ReportFreedSpace
    };
    Kind                     kind = Kind::Expression;
    Token                    token;
    std::string              name;
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
    std::optional<Block> manifest;
    std::optional<Block> detect;
    std::optional<Block> requires_block;
    std::optional<Block> schema;
    std::vector<Command> commands;
};

// Parse one plugin into an AST. Keywords are represented by identifier tokens
// and validated here, keeping lexical evolution separate from the grammar.
Plugin parse(std::string_view source, std::string source_name = {});

} // namespace kap::kpl
