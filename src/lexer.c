
#include <ctype.h>
#include <string.h>

#include "lexer.h"

static char peek(Lexer *lexer)
{
    return lexer->source[lexer->current];
}

static char peek_next(Lexer *lexer)
{
    if (peek(lexer) == '\0')
        return '\0';

    return lexer->source[lexer->current + 1];
}

static char advance(Lexer *lexer)
{
    char c = lexer->source[lexer->current++];

    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }

    return c;
}

static int match(Lexer *lexer, char expected)
{
    if (peek(lexer) != expected)
        return 0;

    advance(lexer);
    return 1;
}

static Token make_token(Lexer *lexer, TokenType type)
{
    Token token;

    token.type = type;
    token.start = lexer->source + lexer->start;
    token.length = lexer->current - lexer->start;
    token.line = lexer->token_line;
    token.column = lexer->token_column;

    return token;
}

static Token error_token(Lexer *lexer)
{
    return make_token(lexer, TOK_ERROR);
}

static void skip_whitespace(Lexer *lexer)
{
    for (;;) {
        char c = peek(lexer);

        switch (c) {
            case ' ':
            case '\r':
            case '\t':
            case '\n':
                advance(lexer);
                break;

            case '/':
                if (peek_next(lexer) == '/') {
                    while (peek(lexer) != '\n' &&
                           peek(lexer) != '\0') {
                        advance(lexer);
                    }
                }
                else if (peek_next(lexer) == '*') {
                    advance(lexer);
                    advance(lexer);

                    while (peek(lexer) != '\0') {
                        if (peek(lexer) == '*' &&
                            peek_next(lexer) == '/') {
                            advance(lexer);
                            advance(lexer);
                            break;
                        }

                        advance(lexer);
                    }
                }
                else {
                    return;
                }
                break;

            default:
                return;
        }
    }
}

static int is_identifier_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static int is_identifier_part(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static TokenType keyword_type(const char *start, size_t length)
{
    #define KEYWORD(word, type) \
        if (length == sizeof(word) - 1 && \
            memcmp(start, word, sizeof(word) - 1) == 0) \
            return type

    KEYWORD("using", TOK_USING);
    KEYWORD("namespace", TOK_NAMESPACE);
    KEYWORD("class", TOK_CLASS);
    KEYWORD("static", TOK_STATIC);
    KEYWORD("void", TOK_VOID);

    KEYWORD("int", TOK_INT);
    KEYWORD("float", TOK_FLOAT);
    KEYWORD("double", TOK_DOUBLE);
    KEYWORD("ulong", TOK_ULONG);
    KEYWORD("char", TOK_CHAR);
    KEYWORD("bool", TOK_BOOL);
    KEYWORD("string", TOK_STRING_TYPE);

    KEYWORD("true", TOK_TRUE);
    KEYWORD("false", TOK_FALSE);

    KEYWORD("if", TOK_IF);
    KEYWORD("else", TOK_ELSE);
    KEYWORD("while", TOK_WHILE);
    KEYWORD("for", TOK_FOR);

    KEYWORD("return", TOK_RETURN);
    KEYWORD("new", TOK_NEW);
    KEYWORD("null", TOK_NULL);
    KEYWORD("break", TOK_BREAK);
    KEYWORD("continue", TOK_CONTINUE);

    #undef KEYWORD

    return TOK_IDENTIFIER;
}

static Token identifier(Lexer *lexer)
{
    while (is_identifier_part(peek(lexer)))
        advance(lexer);

    size_t length = lexer->current - lexer->start;

    TokenType type = keyword_type(
        lexer->source + lexer->start,
        length
    );

    return make_token(lexer, type);
}

static Token number(Lexer *lexer)
{
    while (isdigit((unsigned char)peek(lexer)))
        advance(lexer);

    if (peek(lexer) == '.' &&
        isdigit((unsigned char)peek_next(lexer))) {

        advance(lexer);

        while (isdigit((unsigned char)peek(lexer)))
            advance(lexer);
    }

    return make_token(lexer, TOK_NUMBER);
}

static Token string_literal(Lexer *lexer, int interpolated)
{
    while (peek(lexer) != '"' && peek(lexer) != '\0') {
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            advance(lexer);
            advance(lexer);
        } else {
            advance(lexer);
        }
    }

    if (peek(lexer) == '\0')
        return error_token(lexer);

    advance(lexer);

    return make_token(
        lexer,
        interpolated ? TOK_INTERPOLATED_STRING : TOK_STRING
    );
}

void lexer_init(Lexer *lexer, const char *source)
{
    lexer->source = source;

    lexer->start = 0;
    lexer->current = 0;

    lexer->line = 1;
    lexer->column = 1;

    lexer->token_line = 1;
    lexer->token_column = 1;
}

Token lexer_next_token(Lexer *lexer)
{
    skip_whitespace(lexer);

    lexer->start = lexer->current;

    lexer->token_line = lexer->line;
    lexer->token_column = lexer->column;

    if (peek(lexer) == '\0')
        return make_token(lexer, TOK_EOF);

    char c = advance(lexer);

    if (is_identifier_start(c))
        return identifier(lexer);

    if (isdigit((unsigned char)c))
        return number(lexer);

    switch (c) {
        case '"':
            return string_literal(lexer, 0);

        case '$':
            if (match(lexer, '"'))
                return string_literal(lexer, 1);

            return error_token(lexer);

        case '(':
            return make_token(lexer, TOK_LEFT_PAREN);

        case ')':
            return make_token(lexer, TOK_RIGHT_PAREN);

        case '{':
            return make_token(lexer, TOK_LEFT_BRACE);

        case '}':
            return make_token(lexer, TOK_RIGHT_BRACE);

        case '[':
            return make_token(lexer, TOK_LEFT_BRACKET);

        case ']':
            return make_token(lexer, TOK_RIGHT_BRACKET);

        case ',':
            return make_token(lexer, TOK_COMMA);

        case '.':
            return make_token(lexer, TOK_DOT);

        case ';':
            return make_token(lexer, TOK_SEMICOLON);

        case ':':
            return make_token(lexer, TOK_COLON);

        case '+':
            if (match(lexer, '+'))
                return make_token(lexer, TOK_PLUS_PLUS);

            if (match(lexer, '='))
                return make_token(lexer, TOK_PLUS_EQUAL);

            return make_token(lexer, TOK_PLUS);

        case '-':
            if (match(lexer, '-'))
                return make_token(lexer, TOK_MINUS_MINUS);

            if (match(lexer, '='))
                return make_token(lexer, TOK_MINUS_EQUAL);

            return make_token(lexer, TOK_MINUS);

        case '*':
            if (match(lexer, '='))
                return make_token(lexer, TOK_STAR_EQUAL);

            return make_token(lexer, TOK_STAR);

        case '/':
            if (match(lexer, '='))
                return make_token(lexer, TOK_SLASH_EQUAL);

            return make_token(lexer, TOK_SLASH);

        case '%':
            return make_token(lexer, TOK_PERCENT);

        case '=':
            if (match(lexer, '='))
                return make_token(lexer, TOK_EQUAL_EQUAL);

            return make_token(lexer, TOK_EQUAL);

        case '!':
            if (match(lexer, '='))
                return make_token(lexer, TOK_BANG_EQUAL);

            return make_token(lexer, TOK_BANG);

        case '<':
            if (match(lexer, '='))
                return make_token(lexer, TOK_LESS_EQUAL);

            return make_token(lexer, TOK_LESS);

        case '>':
            if (match(lexer, '='))
                return make_token(lexer, TOK_GREATER_EQUAL);

            return make_token(lexer, TOK_GREATER);

        case '&':
            if (match(lexer, '&'))
                return make_token(lexer, TOK_AND_AND);

            return make_token(lexer, TOK_AMPERSAND);

        case '|':
            if (match(lexer, '|'))
                return make_token(lexer, TOK_OR_OR);

            return make_token(lexer, TOK_PIPE);
    }

    return error_token(lexer);
}
