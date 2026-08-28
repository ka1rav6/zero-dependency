#pragma once

// core/kpl.hpp
//
// The KPL front-end. This first layer turns plugin source into a located token
// stream; the parser in kpl.cpp consumes the same public token types.

#include <cstdint>
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
    TokenKind   kind = TokenKind::End;
    std::string text;
    std::int64_t integer = 0;
    int          line = 1;
    int          column = 1;
};

// Tokenize one KPL source file. Keywords remain Identifier tokens so adding a
// keyword never changes the lexical contract; the parser compares text.
std::vector<Token> lex(std::string_view source, std::string source_name = {});

} // namespace kap::kpl
