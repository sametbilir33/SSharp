
#ifndef SSHARP_PARSER_H
#define SSHARP_PARSER_H

#include "lexer.h"
#include "ast.h"

Stmt *parser_parse(Lexer *lexer);

int parser_had_error(void);

#endif
