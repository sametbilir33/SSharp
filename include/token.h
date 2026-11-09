
#ifndef SSHARP_TOKEN_H
#define SSHARP_TOKEN_H

#include <stddef.h>

typedef enum {
    TOK_EOF,
    TOK_ERROR,

    // Literals
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,
    TOK_INTERPOLATED_STRING,

    // Keywords
    TOK_USING,
    TOK_NAMESPACE,
    TOK_CLASS,
    TOK_STATIC,
    TOK_VOID,
    TOK_INT,
    TOK_FLOAT,
    TOK_DOUBLE,
    TOK_ULONG,
    TOK_CHAR,
    TOK_BOOL,
    TOK_STRING_TYPE,
    TOK_TRUE,
    TOK_FALSE,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_RETURN,
    TOK_NEW,
    TOK_NULL,
    TOK_BREAK,
    TOK_CONTINUE,

    // Delimiters
    TOK_LEFT_PAREN,
    TOK_RIGHT_PAREN,
    TOK_LEFT_BRACE,
    TOK_RIGHT_BRACE,
    TOK_LEFT_BRACKET,
    TOK_RIGHT_BRACKET,

    TOK_COMMA,
    TOK_DOT,
    TOK_SEMICOLON,
    TOK_COLON,

    // Operators
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,

    TOK_EQUAL,
    TOK_EQUAL_EQUAL,

    TOK_BANG,
    TOK_BANG_EQUAL,

    TOK_LESS,
    TOK_LESS_EQUAL,
    TOK_GREATER,
    TOK_GREATER_EQUAL,

    TOK_PLUS_PLUS,
    TOK_MINUS_MINUS,

    TOK_PLUS_EQUAL,
    TOK_MINUS_EQUAL,
    TOK_STAR_EQUAL,
    TOK_SLASH_EQUAL,

    TOK_AND_AND,
    TOK_OR_OR,

    TOK_AMPERSAND,
    TOK_PIPE

} TokenType;

typedef struct {
    TokenType type;

    const char *start;
    size_t length;

    int line;
    int column;

} Token;

const char *token_type_name(TokenType type);

#endif
