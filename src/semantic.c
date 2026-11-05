#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "semantic.h"

typedef struct Symbol {
    Token name;
    DataType type;
    struct Symbol *next;
} Symbol;

typedef struct Scope {
    Symbol *symbols;
    struct Scope *parent;
} Scope;

typedef struct {
    Stmt *declaration;
} FunctionInfo;

typedef struct {
    Scope *scope;
    FunctionInfo *functions;
    size_t function_count;
    size_t function_capacity;
    size_t error_count;
    DataType return_type;
    int in_function;
    size_t scope_depth;
} Analyzer;

static int token_equal(Token left, Token right)
{
    return left.length == right.length &&
        memcmp(left.start, right.start, left.length) == 0;
}

static void report_error(Analyzer *analyzer, Token token, const char *message)
{
    fprintf(stderr,
        "S# Semantic: %s at %d:%d\n",
        message,
        token.line,
        token.column);
    analyzer->error_count++;
}

static void push_scope(Analyzer *analyzer)
{
    Scope *scope = calloc(1, sizeof(Scope));
    if (!scope) {
        fprintf(stderr, "S#: Out of memory.\n");
        exit(EXIT_FAILURE);
    }

    scope->parent = analyzer->scope;
    analyzer->scope = scope;
    analyzer->scope_depth++;
}

static void pop_scope(Analyzer *analyzer)
{
    Scope *scope = analyzer->scope;
    Symbol *symbol = scope->symbols;

    while (symbol) {
        Symbol *next = symbol->next;
        free(symbol);
        symbol = next;
    }

    analyzer->scope = scope->parent;
    free(scope);
    analyzer->scope_depth--;
}

static Symbol *find_symbol_in_scope(Scope *scope, Token name)
{
    for (Symbol *symbol = scope->symbols; symbol; symbol = symbol->next) {
        if (token_equal(symbol->name, name))
            return symbol;
    }

    return NULL;
}

static Symbol *find_symbol(Analyzer *analyzer, Token name)
{
    for (Scope *scope = analyzer->scope; scope; scope = scope->parent) {
        Symbol *symbol = find_symbol_in_scope(scope, name);
        if (symbol)
            return symbol;
    }

    return NULL;
}

static int declare_symbol(
    Analyzer *analyzer,
    Token name,
    DataType type,
    int check_duplicate)
{
    if (check_duplicate &&
        find_symbol_in_scope(analyzer->scope, name)) {
        report_error(analyzer, name, "Duplicate declaration");
        return 0;
    }

    Symbol *symbol = malloc(sizeof(Symbol));
    if (!symbol) {
        fprintf(stderr, "S#: Out of memory.\n");
        exit(EXIT_FAILURE);
    }

    symbol->name = name;
    symbol->type = type;
    symbol->next = analyzer->scope->symbols;
    analyzer->scope->symbols = symbol;
    return 1;
}

static Stmt *find_function(Analyzer *analyzer, Token name)
{
    for (size_t i = 0; i < analyzer->function_count; i++) {
        Stmt *function = analyzer->functions[i].declaration;
        if (token_equal(function->function.name, name))
            return function;
    }

    return NULL;
}

static int is_numeric(DataType type)
{
    return type == TYPE_INT ||
        type == TYPE_FLOAT ||
        type == TYPE_DOUBLE;
}

static DataType promote_numeric(DataType left, DataType right)
{
    if (left == TYPE_DOUBLE || right == TYPE_DOUBLE)
        return TYPE_DOUBLE;
    if (left == TYPE_FLOAT || right == TYPE_FLOAT)
        return TYPE_FLOAT;
    return TYPE_INT;
}

static int can_assign(DataType target, DataType value)
{
    if (target == value)
        return 1;
    if (target == TYPE_FLOAT && value == TYPE_INT)
        return 1;
    if (target == TYPE_DOUBLE &&
        (value == TYPE_INT || value == TYPE_FLOAT))
        return 1;
    if (target == TYPE_STRING && value == TYPE_NULL)
        return 1;
    return 0;
}

static Token expression_token(Expr *expr)
{
    if (!expr)
        return (Token){0};

    switch (expr->type) {
        case EXPR_LITERAL:
            return expr->literal.value;
        case EXPR_VARIABLE:
            return expr->variable.name;
        case EXPR_BINARY:
            return expr->binary.operator;
        case EXPR_UNARY:
            return expr->unary.operator;
        case EXPR_ASSIGN:
            return expr->assign.name;
        case EXPR_CALL:
            return expression_token(expr->call.callee);
        case EXPR_MEMBER:
            return expr->member.name;
    }

    return (Token){0};
}

static DataType analyze_expression(Analyzer *analyzer, Expr *expr);
static void analyze_statement(Analyzer *analyzer, Stmt *stmt);

static DataType analyze_call(Analyzer *analyzer, Expr *expr)
{
    Expr *callee = expr->call.callee;

    if (callee && callee->type == EXPR_VARIABLE) {
        Token name = callee->variable.name;
        if (find_symbol(analyzer, name)) {
            report_error(analyzer, name, "Variable is not callable");
            for (size_t i = 0; i < expr->call.argument_count; i++)
                analyze_expression(analyzer, expr->call.arguments[i]);
            return TYPE_ERROR;
        }

        Stmt *function = find_function(analyzer, name);

        if (!function) {
            report_error(analyzer, name, "Undefined function");

            for (size_t i = 0; i < expr->call.argument_count; i++)
                analyze_expression(analyzer, expr->call.arguments[i]);
            return TYPE_ERROR;
        }

        if (expr->call.argument_count != function->function.parameter_count)
            report_error(analyzer, name, "Incorrect number of function arguments");

        size_t count = expr->call.argument_count;
        if (count > function->function.parameter_count)
            count = function->function.parameter_count;

        for (size_t i = 0; i < expr->call.argument_count; i++) {
            DataType argument_type =
                analyze_expression(analyzer, expr->call.arguments[i]);

            if (i < count &&
                argument_type != TYPE_ERROR &&
                !can_assign(
                    function->function.parameters[i].data_type,
                    argument_type)) {
                report_error(
                    analyzer,
                    expression_token(expr->call.arguments[i]),
                    "Function argument type mismatch");
            }
        }

        return function->function.return_type;
    }

    if (callee && callee->type == EXPR_MEMBER) {
        for (size_t i = 0; i < expr->call.argument_count; i++)
            analyze_expression(analyzer, expr->call.arguments[i]);
        return TYPE_VOID;
    }

    if (callee)
        analyze_expression(analyzer, callee);
    for (size_t i = 0; i < expr->call.argument_count; i++)
        analyze_expression(analyzer, expr->call.arguments[i]);
    report_error(
        analyzer,
        expression_token(callee),
        "Expression is not callable"
    );
    return TYPE_ERROR;
}

static DataType analyze_expression(Analyzer *analyzer, Expr *expr)
{
    if (!expr)
        return TYPE_VOID;

    switch (expr->type) {
        case EXPR_LITERAL:
            return expr->literal.data_type;

        case EXPR_VARIABLE: {
            Symbol *symbol = find_symbol(analyzer, expr->variable.name);
            if (!symbol) {
                report_error(
                    analyzer,
                    expr->variable.name,
                    "Undefined variable"
                );
                return TYPE_ERROR;
            }
            return symbol->type;
        }

        case EXPR_BINARY: {
            DataType left = analyze_expression(analyzer, expr->binary.left);
            DataType right = analyze_expression(analyzer, expr->binary.right);
            TokenType op = expr->binary.operator.type;

            if (left == TYPE_ERROR || right == TYPE_ERROR)
                return TYPE_ERROR;
            if (left == TYPE_VOID || right == TYPE_VOID) {
                report_error(
                    analyzer,
                    expr->binary.operator,
                    "void expression cannot be used as an operand"
                );
                return TYPE_ERROR;
            }

            if (op == TOK_AND_AND || op == TOK_OR_OR) {
                if (left != TYPE_BOOL || right != TYPE_BOOL)
                    report_error(
                        analyzer,
                        expr->binary.operator,
                        "Logical operators require bool operands"
                    );
                return TYPE_BOOL;
            }

            if (op == TOK_EQUAL_EQUAL || op == TOK_BANG_EQUAL) {
                if (!((is_numeric(left) && is_numeric(right)) ||
                    left == right ||
                    (left == TYPE_NULL && right == TYPE_STRING) ||
                    (left == TYPE_STRING && right == TYPE_NULL))) {
                    report_error(
                        analyzer,
                        expr->binary.operator,
                        "Incompatible operands for equality comparison"
                    );
                }
                return TYPE_BOOL;
            }

            if (op == TOK_LESS || op == TOK_LESS_EQUAL ||
                op == TOK_GREATER || op == TOK_GREATER_EQUAL) {
                if (!((is_numeric(left) && is_numeric(right)) ||
                    (left == TYPE_STRING && right == TYPE_STRING))) {
                    report_error(
                        analyzer,
                        expr->binary.operator,
                        "Incompatible operands for comparison"
                    );
                }
                return TYPE_BOOL;
            }

            if (op == TOK_PLUS &&
                left == TYPE_STRING && right == TYPE_STRING)
                return TYPE_STRING;

            if (!is_numeric(left) || !is_numeric(right)) {
                report_error(
                    analyzer,
                    expr->binary.operator,
                    "Arithmetic operators require numeric operands"
                );
                return TYPE_ERROR;
            }
            return promote_numeric(left, right);
        }

        case EXPR_UNARY: {
            DataType operand = analyze_expression(analyzer, expr->unary.right);
            if (operand == TYPE_ERROR)
                return TYPE_ERROR;
            if (operand == TYPE_VOID) {
                report_error(
                    analyzer,
                    expr->unary.operator,
                    "void expression cannot be used as an operand"
                );
                return TYPE_ERROR;
            }

            if (expr->unary.operator.type == TOK_BANG) {
                if (operand != TYPE_BOOL)
                    report_error(
                        analyzer,
                        expr->unary.operator,
                        "Logical negation requires a bool operand"
                    );
                return TYPE_BOOL;
            }

            if (!is_numeric(operand))
                report_error(
                    analyzer,
                    expr->unary.operator,
                    "Unary arithmetic operators require a numeric operand"
                );
            return is_numeric(operand) ? operand : TYPE_ERROR;
        }

        case EXPR_ASSIGN: {
            Symbol *symbol = find_symbol(analyzer, expr->assign.name);
            DataType value = analyze_expression(analyzer, expr->assign.value);

            if (!symbol) {
                report_error(
                    analyzer,
                    expr->assign.name,
                    "Assignment to undefined variable"
                );
                return TYPE_VOID;
            }

            if (value != TYPE_ERROR && !can_assign(symbol->type, value))
                report_error(
                    analyzer,
                    expr->assign.name,
                    "Assignment type mismatch"
                );
            return symbol->type;
        }

        case EXPR_CALL:
            return analyze_call(analyzer, expr);

        case EXPR_MEMBER:
            report_error(
                analyzer,
                expr->member.name,
                "Member access is only supported as a function call"
            );
            return TYPE_ERROR;
    }

    return TYPE_VOID;
}

static void analyze_block_contents(Analyzer *analyzer, Stmt *block)
{
    for (size_t i = 0; i < block->block.count; i++)
        analyze_statement(analyzer, block->block.statements[i]);
}

static int statement_always_returns(Stmt *stmt)
{
    if (!stmt)
        return 0;

    switch (stmt->type) {
        case STMT_RETURN:
            return 1;

        case STMT_BLOCK:
            for (size_t i = 0; i < stmt->block.count; i++) {
                if (statement_always_returns(stmt->block.statements[i]))
                    return 1;
            }
            return 0;

        case STMT_IF:
            return stmt->if_stmt.else_branch &&
                statement_always_returns(stmt->if_stmt.then_branch) &&
                statement_always_returns(stmt->if_stmt.else_branch);

        case STMT_EXPRESSION:
        case STMT_VARIABLE:
        case STMT_WHILE:
        case STMT_FOR:
        case STMT_FUNCTION:
            return 0;
    }

    return 0;
}

static void analyze_function(Analyzer *analyzer, Stmt *stmt)
{
    DataType previous_return_type = analyzer->return_type;
    int previous_in_function = analyzer->in_function;

    analyzer->return_type = stmt->function.return_type;
    analyzer->in_function = 1;
    push_scope(analyzer);

    for (size_t i = 0; i < stmt->function.parameter_count; i++) {
        FunctionParameter parameter = stmt->function.parameters[i];
        declare_symbol(analyzer, parameter.name, parameter.data_type, 1);
    }

    if (stmt->function.body)
        analyze_block_contents(analyzer, stmt->function.body);

    if (stmt->function.return_type != TYPE_VOID &&
        stmt->function.body &&
        !statement_always_returns(stmt->function.body)) {
        report_error(
            analyzer,
            stmt->function.name,
            "Not all code paths return a value"
        );
    }

    pop_scope(analyzer);
    analyzer->return_type = previous_return_type;
    analyzer->in_function = previous_in_function;
}

static void analyze_statement(Analyzer *analyzer, Stmt *stmt)
{
    if (!stmt)
        return;

    switch (stmt->type) {
        case STMT_EXPRESSION:
            analyze_expression(analyzer, stmt->expression.expression);
            break;

        case STMT_VARIABLE: {
            if (stmt->variable.data_type == TYPE_VOID) {
                report_error(
                    analyzer,
                    stmt->variable.name,
                    "Variable cannot have type void"
                );
            }

            if (stmt->variable.initializer) {
                DataType initializer = analyze_expression(
                    analyzer,
                    stmt->variable.initializer
                );
                if (initializer != TYPE_ERROR &&
                    !can_assign(stmt->variable.data_type, initializer)) {
                    report_error(
                        analyzer,
                        stmt->variable.name,
                        "Variable initializer type mismatch"
                    );
                }
            }

            if (analyzer->scope_depth == 1 &&
                find_function(analyzer, stmt->variable.name)) {
                report_error(
                    analyzer,
                    stmt->variable.name,
                    "Variable conflicts with function declaration"
                );
            }

            declare_symbol(
                analyzer,
                stmt->variable.name,
                stmt->variable.data_type,
                1
            );
            break;
        }

        case STMT_BLOCK:
            push_scope(analyzer);
            analyze_block_contents(analyzer, stmt);
            pop_scope(analyzer);
            break;

        case STMT_IF: {
            DataType condition =
                analyze_expression(analyzer, stmt->if_stmt.condition);
            if (condition != TYPE_BOOL && condition != TYPE_ERROR)
                report_error(
                    analyzer,
                    expression_token(stmt->if_stmt.condition),
                    "if condition must have type bool"
                );
            analyze_statement(analyzer, stmt->if_stmt.then_branch);
            analyze_statement(analyzer, stmt->if_stmt.else_branch);
            break;
        }

        case STMT_WHILE: {
            DataType condition =
                analyze_expression(analyzer, stmt->while_stmt.condition);
            if (condition != TYPE_BOOL && condition != TYPE_ERROR)
                report_error(
                    analyzer,
                    expression_token(stmt->while_stmt.condition),
                    "while condition must have type bool"
                );
            analyze_statement(analyzer, stmt->while_stmt.body);
            break;
        }

        case STMT_FOR:
            push_scope(analyzer);
            analyze_statement(analyzer, stmt->for_stmt.initializer);
            if (stmt->for_stmt.condition) {
                DataType condition =
                    analyze_expression(analyzer, stmt->for_stmt.condition);
                if (condition != TYPE_BOOL && condition != TYPE_ERROR)
                    report_error(
                        analyzer,
                        expression_token(stmt->for_stmt.condition),
                        "for condition must have type bool"
                    );
            }
            analyze_expression(analyzer, stmt->for_stmt.increment);
            analyze_statement(analyzer, stmt->for_stmt.body);
            pop_scope(analyzer);
            break;

        case STMT_RETURN:
            if (!analyzer->in_function) {
                report_error(
                    analyzer,
                    stmt->return_stmt.keyword,
                    "return statement is only valid inside a function"
                );
                if (stmt->return_stmt.value)
                    analyze_expression(analyzer, stmt->return_stmt.value);
                break;
            }

            if (analyzer->return_type == TYPE_VOID) {
                if (stmt->return_stmt.value) {
                    analyze_expression(analyzer, stmt->return_stmt.value);
                    report_error(
                        analyzer,
                        expression_token(stmt->return_stmt.value),
                        "void function cannot return a value"
                    );
                }
            } else if (!stmt->return_stmt.value) {
                report_error(
                    analyzer,
                    stmt->return_stmt.keyword,
                    "Non-void function must return a value"
                );
            } else {
                DataType value =
                    analyze_expression(analyzer, stmt->return_stmt.value);
                if (value != TYPE_ERROR &&
                    !can_assign(analyzer->return_type, value)) {
                    report_error(
                        analyzer,
                        expression_token(stmt->return_stmt.value),
                        "Return value type mismatch"
                    );
                }
            }
            break;

        case STMT_FUNCTION:
            if (analyzer->scope_depth != 1)
                report_error(
                    analyzer,
                    stmt->function.name,
                    "Function declarations are only allowed at top level"
                );
            analyze_function(analyzer, stmt);
            break;
    }
}

static void collect_functions(Analyzer *analyzer, Stmt *program)
{
    for (size_t i = 0; i < program->block.count; i++) {
        Stmt *stmt = program->block.statements[i];
        if (stmt->type != STMT_FUNCTION)
            continue;

        if (find_function(analyzer, stmt->function.name)) {
            report_error(
                analyzer,
                stmt->function.name,
                "Duplicate function declaration"
            );
            continue;
        }

        if (analyzer->function_count == analyzer->function_capacity) {
            size_t capacity = analyzer->function_capacity == 0
                ? 8
                : analyzer->function_capacity * 2;
            FunctionInfo *functions = realloc(
                analyzer->functions,
                capacity * sizeof(FunctionInfo)
            );
            if (!functions) {
                fprintf(stderr, "S#: Out of memory.\n");
                exit(EXIT_FAILURE);
            }
            analyzer->functions = functions;
            analyzer->function_capacity = capacity;
        }

        analyzer->functions[analyzer->function_count++].declaration = stmt;
    }
}

int semantic_analyze(Stmt *program)
{
    if (!program || program->type != STMT_BLOCK)
        return 0;

    Analyzer analyzer = {0};
    push_scope(&analyzer);
    collect_functions(&analyzer, program);

    analyze_block_contents(&analyzer, program);

    pop_scope(&analyzer);
    free(analyzer.functions);
    return analyzer.error_count == 0;
}
