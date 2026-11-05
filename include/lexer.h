
#ifndef SSHARP_LEXER_H
#define SSHARP_LEXER_H

#include "token.h"

typedef struct {
    const char *source;

    size_t start;
    size_t current;

    int line;
    int column;

    int token_line;
    int token_column;

} Lexer;

void lexer_init(Lexer *lexer, const char *source);

Token lexer_next_token(Lexer *lexer);

#endif
