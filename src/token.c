
#include "token.h"

const char *token_type_name(TokenType type)
{
    static const char *names[] = {
        "EOF",
        "ERROR",

        "IDENTIFIER",
        "NUMBER",
        "STRING",
        "INTERPOLATED_STRING",

        "USING",
        "NAMESPACE",
        "CLASS",
        "STATIC",
        "VOID",
        "INT",
        "FLOAT",
        "DOUBLE",
        "ULONG",
        "CHAR",
        "BOOL",
        "STRING_TYPE",
        "TRUE",
        "FALSE",
        "IF",
        "ELSE",
        "WHILE",
        "FOR",
        "RETURN",
        "NEW",
        "NULL",
        "BREAK",
        "CONTINUE",

        "LEFT_PAREN",
        "RIGHT_PAREN",
        "LEFT_BRACE",
        "RIGHT_BRACE",
        "LEFT_BRACKET",
        "RIGHT_BRACKET",

        "COMMA",
        "DOT",
        "SEMICOLON",
        "COLON",

        "PLUS",
        "MINUS",
        "STAR",
        "SLASH",
        "PERCENT",

        "EQUAL",
        "EQUAL_EQUAL",

        "BANG",
        "BANG_EQUAL",

        "LESS",
        "LESS_EQUAL",
        "GREATER",
        "GREATER_EQUAL",

        "PLUS_PLUS",
        "MINUS_MINUS",

        "PLUS_EQUAL",
        "MINUS_EQUAL",
        "STAR_EQUAL",
        "SLASH_EQUAL",

        "AND_AND",
        "OR_OR",

        "AMPERSAND",
        "PIPE"
    };

    size_t count = sizeof(names) / sizeof(names[0]);

    if ((int)type < 0 || (size_t)type >= count)
        return "UNKNOWN";

    return names[type];
}
