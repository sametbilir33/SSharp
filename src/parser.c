
#include <stdio.h>
#include <stdlib.h>

#include "parser.h"

static Lexer *lexer;
static Token current;
static Token previous;
static int had_error = 0;

static void advance_token(void)
{
    previous = current;

    do {
        current = lexer_next_token(lexer);

        if (current.type == TOK_ERROR) {
            fprintf(stderr,
                "S# Parser: Invalid token at %d:%d\n",
                current.line,
                current.column);

            had_error = 1;
        }

    } while (current.type == TOK_ERROR);
}

static int check(TokenType type)
{
    return current.type == type;
}

static int match(TokenType type)
{
    if (!check(type))
        return 0;

    advance_token();
    return 1;
}

static Token consume(TokenType type, const char *message)
{
    if (check(type)) {
        Token token = current;
        advance_token();
        return token;
    }

    fprintf(stderr,
        "S# Parser: %s at %d:%d\n",
        message,
        current.line,
        current.column);

    had_error = 1;

    return current;
}

static Expr *new_expr(ExprType type)
{
    Expr *expr = calloc(1, sizeof(Expr));

    if (!expr) {
        fprintf(stderr, "S#: Out of memory.\n");
        exit(EXIT_FAILURE);
    }

    expr->type = type;
    return expr;
}

static Stmt *new_stmt(StmtType type)
{
    Stmt *stmt = calloc(1, sizeof(Stmt));

    if (!stmt) {
        fprintf(stderr, "S#: Out of memory.\n");
        exit(EXIT_FAILURE);
    }

    stmt->type = type;
    return stmt;
}

static Expr *parse_expression(void);
static Stmt *parse_statement(void);
static Stmt *parse_declaration(void);
static Stmt *parse_block(void);

static Expr *parse_primary(void)
{
    if (match(TOK_NUMBER) ||
        match(TOK_STRING) ||
        match(TOK_INTERPOLATED_STRING) ||
        match(TOK_TRUE) ||
        match(TOK_FALSE) ||
        match(TOK_NULL)) {

        Expr *expr = new_expr(EXPR_LITERAL);
        expr->literal.value = previous;

        switch (previous.type) {
            case TOK_NUMBER: {
                expr->literal.data_type = TYPE_INT;
                for (size_t i = 0; i < previous.length; i++) {
                    if (previous.start[i] == '.') {
                        expr->literal.data_type = TYPE_DOUBLE;
                        break;
                    }
                }
                break;
            }

            case TOK_STRING:
            case TOK_INTERPOLATED_STRING:
                expr->literal.data_type = TYPE_STRING;
                break;

            case TOK_TRUE:
            case TOK_FALSE:
                expr->literal.data_type = TYPE_BOOL;
                break;

            case TOK_NULL:
                expr->literal.data_type = TYPE_NULL;
                break;

            default:
                expr->literal.data_type = TYPE_VOID;
                break;
        }

        return expr;
    }

    if (match(TOK_IDENTIFIER)) {
        Expr *expr = new_expr(EXPR_VARIABLE);
        expr->variable.name = previous;
        return expr;
    }

    if (match(TOK_LEFT_PAREN)) {
        Expr *expr = parse_expression();

        consume(TOK_RIGHT_PAREN, "Expected ')' after expression");

        return expr;
    }

    fprintf(stderr,
        "S# Parser: Expected expression at %d:%d\n",
        current.line,
        current.column);

    had_error = 1;

    return NULL;
}

static Expr *parse_postfix(void)
{
    Expr *expr = parse_primary();

    while (!had_error && expr) {
        if (match(TOK_DOT)) {
            Token name = consume(
                TOK_IDENTIFIER,
                "Expected member name after '.'"
            );

            Expr *member = new_expr(EXPR_MEMBER);

            member->member.object = expr;
            member->member.name = name;

            expr = member;
        }
        else if (match(TOK_LEFT_PAREN)) {
            Expr *call = new_expr(EXPR_CALL);

            call->call.callee = expr;

            size_t capacity = 0;

            if (!check(TOK_RIGHT_PAREN)) {
                do {
                    if (call->call.argument_count == capacity) {
                        size_t new_capacity = capacity == 0
                            ? 4
                            : capacity * 2;

                        Expr **arguments = realloc(
                            call->call.arguments,
                            new_capacity * sizeof(Expr *)
                        );

                        if (!arguments) {
                            fprintf(stderr, "S#: Out of memory.\n");
                            exit(EXIT_FAILURE);
                        }

                        call->call.arguments = arguments;
                        capacity = new_capacity;
                    }

                    Expr *argument = parse_expression();

                    if (!argument) {
                        ast_free_expr(call);
                        return NULL;
                    }

                    call->call.arguments[
                        call->call.argument_count++
                    ] = argument;

                } while (match(TOK_COMMA));
            }

            consume(TOK_RIGHT_PAREN, "Expected ')' after arguments");

            expr = call;
        }
        else {
            break;
        }
    }

    return expr;
}

static Expr *parse_unary(void)
{
    if (match(TOK_BANG) ||
        match(TOK_MINUS) ||
        match(TOK_PLUS)) {

        Token op = previous;
        Expr *right = parse_unary();

        if (!right)
            return NULL;

        Expr *expr = new_expr(EXPR_UNARY);

        expr->unary.operator = op;
        expr->unary.right = right;

        return expr;
    }

    return parse_postfix();
}

static Expr *make_binary(Expr *left, Token op, Expr *right)
{
    if (!left || !right) {
        ast_free_expr(left);
        ast_free_expr(right);
        return NULL;
    }

    Expr *expr = new_expr(EXPR_BINARY);

    expr->binary.left = left;
    expr->binary.operator = op;
    expr->binary.right = right;

    return expr;
}

static Expr *parse_factor(void)
{
    Expr *expr = parse_unary();

    while (!had_error &&
        (check(TOK_STAR) ||
         check(TOK_SLASH) ||
         check(TOK_PERCENT))) {

        advance_token();
        Token op = previous;

        Expr *right = parse_unary();

        expr = make_binary(expr, op, right);

        if (!expr)
            return NULL;
    }

    return expr;
}

static Expr *parse_term(void)
{
    Expr *expr = parse_factor();

    while (!had_error &&
        (check(TOK_PLUS) || check(TOK_MINUS))) {

        advance_token();
        Token op = previous;

        Expr *right = parse_factor();

        expr = make_binary(expr, op, right);

        if (!expr)
            return NULL;
    }

    return expr;
}

static Expr *parse_comparison(void)
{
    Expr *expr = parse_term();

    while (!had_error &&
        (check(TOK_LESS) ||
         check(TOK_LESS_EQUAL) ||
         check(TOK_GREATER) ||
         check(TOK_GREATER_EQUAL))) {

        advance_token();
        Token op = previous;

        Expr *right = parse_term();

        expr = make_binary(expr, op, right);

        if (!expr)
            return NULL;
    }

    return expr;
}

static Expr *parse_equality(void)
{
    Expr *expr = parse_comparison();

    while (!had_error &&
        (check(TOK_EQUAL_EQUAL) ||
         check(TOK_BANG_EQUAL))) {

        advance_token();
        Token op = previous;

        Expr *right = parse_comparison();

        expr = make_binary(expr, op, right);

        if (!expr)
            return NULL;
    }

    return expr;
}

static Expr *parse_and(void)
{
    Expr *expr = parse_equality();

    while (!had_error && check(TOK_AND_AND)) {
        advance_token();
        Token op = previous;

        Expr *right = parse_equality();

        expr = make_binary(expr, op, right);

        if (!expr)
            return NULL;
    }

    return expr;
}

static Expr *parse_or(void)
{
    Expr *expr = parse_and();

    while (!had_error && check(TOK_OR_OR)) {
        advance_token();
        Token op = previous;

        Expr *right = parse_and();

        expr = make_binary(expr, op, right);

        if (!expr)
            return NULL;
    }

    return expr;
}

static Expr *parse_assignment(void)
{
    Expr *expr = parse_or();

    if (match(TOK_EQUAL)) {
        Token equals = previous;

        Expr *value = parse_assignment();

        if (!expr || expr->type != EXPR_VARIABLE) {
            fprintf(stderr,
                "S# Parser: Invalid assignment target at %d:%d\n",
                equals.line,
                equals.column);

            had_error = 1;
            ast_free_expr(expr);
            ast_free_expr(value);
            return NULL;
        }

        if (!value) {
            ast_free_expr(expr);
            return NULL;
        }

        Expr *assign = new_expr(EXPR_ASSIGN);

        assign->assign.name = expr->variable.name;
        assign->assign.value = value;

        free(expr);

        return assign;
    }

    return expr;
}

static Expr *parse_expression(void)
{
    return parse_assignment();
}

static int is_type_token(TokenType type)
{
    return type == TOK_INT ||
           type == TOK_FLOAT ||
           type == TOK_DOUBLE ||
           type == TOK_BOOL ||
           type == TOK_STRING_TYPE;
}

static int is_return_type_token(TokenType type)
{
    return is_type_token(type) || type == TOK_VOID;
}

static DataType parse_type(void)
{
    switch (current.type) {
        case TOK_VOID:
            advance_token();
            return TYPE_VOID;

        case TOK_INT:
            advance_token();
            return TYPE_INT;

        case TOK_FLOAT:
            advance_token();
            return TYPE_FLOAT;

        case TOK_DOUBLE:
            advance_token();
            return TYPE_DOUBLE;

        case TOK_BOOL:
            advance_token();
            return TYPE_BOOL;

        case TOK_STRING_TYPE:
            advance_token();
            return TYPE_STRING;

        default:
            return TYPE_VOID;
    }
}

static Stmt *parse_variable_after_name(
    DataType type,
    Token name,
    int consume_semicolon)
{
    Expr *initializer = NULL;

    if (match(TOK_EQUAL)) {
        initializer = parse_expression();

        if (!initializer)
            return NULL;
    }

    if (consume_semicolon)
        consume(TOK_SEMICOLON, "Expected ';' after variable declaration");

    Stmt *stmt = new_stmt(STMT_VARIABLE);

    stmt->variable.data_type = type;
    stmt->variable.name = name;
    stmt->variable.initializer = initializer;

    return stmt;
}

static Stmt *parse_function_declaration(DataType return_type, Token name)
{
    Stmt *stmt = new_stmt(STMT_FUNCTION);
    stmt->function.return_type = return_type;
    stmt->function.name = name;

    consume(TOK_LEFT_PAREN, "Expected '(' after function name");

    size_t capacity = 0;
    if (!check(TOK_RIGHT_PAREN)) {
        do {
            if (!is_type_token(current.type)) {
                fprintf(stderr,
                    "S# Parser: Expected parameter type at %d:%d\n",
                    current.line,
                    current.column);
                had_error = 1;
                break;
            }

            DataType parameter_type = parse_type();
            Token parameter_name = consume(
                TOK_IDENTIFIER,
                "Expected parameter name"
            );

            if (stmt->function.parameter_count == capacity) {
                size_t new_capacity = capacity == 0 ? 4 : capacity * 2;
                FunctionParameter *parameters = realloc(
                    stmt->function.parameters,
                    new_capacity * sizeof(FunctionParameter)
                );

                if (!parameters) {
                    fprintf(stderr, "S#: Out of memory.\n");
                    exit(EXIT_FAILURE);
                }

                stmt->function.parameters = parameters;
                capacity = new_capacity;
            }

            stmt->function.parameters[
                stmt->function.parameter_count++
            ] = (FunctionParameter){parameter_type, parameter_name};
        } while (match(TOK_COMMA) && !had_error);
    }

    consume(TOK_RIGHT_PAREN, "Expected ')' after function parameters");
    consume(TOK_LEFT_BRACE, "Expected '{' before function body");

    if (!had_error)
        stmt->function.body = parse_block();

    if (had_error) {
        ast_free_stmt(stmt);
        return NULL;
    }

    return stmt;
}

static Stmt *parse_typed_declaration(void)
{
    DataType type = parse_type();
    Token name = consume(TOK_IDENTIFIER, "Expected declaration name");

    if (check(TOK_LEFT_PAREN))
        return parse_function_declaration(type, name);

    return parse_variable_after_name(type, name, 1);
}

static Stmt *parse_block(void)
{
    Stmt *block = new_stmt(STMT_BLOCK);

    size_t capacity = 0;

    while (!check(TOK_RIGHT_BRACE) &&
           !check(TOK_EOF) &&
           !had_error) {

        Stmt *statement = parse_declaration();

        if (!statement)
            break;

        if (block->block.count == capacity) {
            size_t new_capacity = capacity == 0
                ? 4
                : capacity * 2;

            Stmt **statements = realloc(
                block->block.statements,
                new_capacity * sizeof(Stmt *)
            );

            if (!statements) {
                fprintf(stderr, "S#: Out of memory.\n");
                exit(EXIT_FAILURE);
            }

            block->block.statements = statements;
            capacity = new_capacity;
        }

        block->block.statements[
            block->block.count++
        ] = statement;
    }

    consume(TOK_RIGHT_BRACE, "Expected '}' after block");

    return block;
}

static Stmt *parse_if_statement(void)
{
    consume(TOK_LEFT_PAREN, "Expected '(' after if");

    Expr *condition = parse_expression();

    consume(TOK_RIGHT_PAREN, "Expected ')' after condition");

    if (!condition)
        return NULL;

    Stmt *then_branch = parse_statement();

    if (!then_branch) {
        ast_free_expr(condition);
        return NULL;
    }

    Stmt *else_branch = NULL;

    if (match(TOK_ELSE))
        else_branch = parse_statement();

    Stmt *stmt = new_stmt(STMT_IF);

    stmt->if_stmt.condition = condition;
    stmt->if_stmt.then_branch = then_branch;
    stmt->if_stmt.else_branch = else_branch;

    return stmt;
}

static Stmt *parse_while_statement(void)
{
    consume(TOK_LEFT_PAREN, "Expected '(' after while");
    Expr *condition = parse_expression();
    consume(TOK_RIGHT_PAREN, "Expected ')' after while condition");

    if (!condition)
        return NULL;

    Stmt *body = parse_statement();
    if (!body) {
        ast_free_expr(condition);
        return NULL;
    }

    Stmt *stmt = new_stmt(STMT_WHILE);
    stmt->while_stmt.condition = condition;
    stmt->while_stmt.body = body;
    return stmt;
}

static Stmt *parse_for_statement(void)
{
    consume(TOK_LEFT_PAREN, "Expected '(' after for");

    Stmt *initializer = NULL;
    if (match(TOK_SEMICOLON)) {
        initializer = NULL;
    } else if (is_type_token(current.type)) {
        DataType type = parse_type();
        Token name = consume(TOK_IDENTIFIER, "Expected variable name");
        initializer = parse_variable_after_name(type, name, 1);
    } else {
        Expr *expression = parse_expression();
        consume(TOK_SEMICOLON, "Expected ';' after for initializer");

        if (expression) {
            initializer = new_stmt(STMT_EXPRESSION);
            initializer->expression.expression = expression;
        }
    }

    Expr *condition = NULL;
    if (!check(TOK_SEMICOLON))
        condition = parse_expression();
    consume(TOK_SEMICOLON, "Expected ';' after for condition");

    Expr *increment = NULL;
    if (!check(TOK_RIGHT_PAREN))
        increment = parse_expression();
    consume(TOK_RIGHT_PAREN, "Expected ')' after for clauses");

    if (had_error) {
        ast_free_stmt(initializer);
        ast_free_expr(condition);
        ast_free_expr(increment);
        return NULL;
    }

    Stmt *body = parse_statement();
    if (!body) {
        ast_free_stmt(initializer);
        ast_free_expr(condition);
        ast_free_expr(increment);
        return NULL;
    }

    Stmt *stmt = new_stmt(STMT_FOR);
    stmt->for_stmt.initializer = initializer;
    stmt->for_stmt.condition = condition;
    stmt->for_stmt.increment = increment;
    stmt->for_stmt.body = body;
    return stmt;
}

static Stmt *parse_return_statement(void)
{
    Token keyword = previous;
    Expr *value = NULL;
    if (!check(TOK_SEMICOLON))
        value = parse_expression();

    consume(TOK_SEMICOLON, "Expected ';' after return statement");

    if (had_error) {
        ast_free_expr(value);
        return NULL;
    }

    Stmt *stmt = new_stmt(STMT_RETURN);
    stmt->return_stmt.keyword = keyword;
    stmt->return_stmt.value = value;
    return stmt;
}

static Stmt *parse_statement(void)
{
    if (match(TOK_IF))
        return parse_if_statement();

    if (match(TOK_WHILE))
        return parse_while_statement();

    if (match(TOK_FOR))
        return parse_for_statement();

    if (match(TOK_RETURN))
        return parse_return_statement();

    if (match(TOK_LEFT_BRACE))
        return parse_block();

    Expr *expr = parse_expression();

    if (!expr)
        return NULL;

    consume(TOK_SEMICOLON, "Expected ';' after expression");

    Stmt *stmt = new_stmt(STMT_EXPRESSION);

    stmt->expression.expression = expr;

    return stmt;
}

static void skip_using_directive(void)
{
    advance_token();

    while (!check(TOK_SEMICOLON) &&
           !check(TOK_EOF) &&
           !had_error) {
        advance_token();
    }

    consume(TOK_SEMICOLON, "Expected ';' after using directive");
}

static Stmt *parse_declaration(void)
{
    if (match(TOK_USING)) {
        previous = current;
        skip_using_directive();
        return NULL;
    }

    if (is_return_type_token(current.type))
        return parse_typed_declaration();

    return parse_statement();
}

Stmt *parser_parse(Lexer *source_lexer)
{
    lexer = source_lexer;
    had_error = 0;

    advance_token();

    Stmt *program = new_stmt(STMT_BLOCK);

    size_t capacity = 0;

    while (!check(TOK_EOF) && !had_error) {
        Stmt *statement = parse_declaration();

        if (!statement)
            continue;

        if (program->block.count == capacity) {
            size_t new_capacity = capacity == 0
                ? 8
                : capacity * 2;

            Stmt **statements = realloc(
                program->block.statements,
                new_capacity * sizeof(Stmt *)
            );

            if (!statements) {
                fprintf(stderr, "S#: Out of memory.\n");
                exit(EXIT_FAILURE);
            }

            program->block.statements = statements;
            capacity = new_capacity;
        }

        program->block.statements[
            program->block.count++
        ] = statement;
    }

    return program;
}

int parser_had_error(void)
{
    return had_error;
}
