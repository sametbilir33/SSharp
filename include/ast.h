
#ifndef SSHARP_AST_H
#define SSHARP_AST_H

#include <stddef.h>
#include "token.h"

typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_UINT64,
    TYPE_CHAR,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_VOID,
    TYPE_NULL,
    TYPE_ERROR,
    TYPE_INT_ARRAY,
    TYPE_BOOL_ARRAY,
    TYPE_STRING_ARRAY
} DataType;

typedef enum {
    EXPR_LITERAL,
    EXPR_VARIABLE,
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_ASSIGN,
    EXPR_CALL,
    EXPR_MEMBER,
    EXPR_INDEX,
    EXPR_ARRAY_LITERAL,
    EXPR_ARRAY_CREATE
} ExprType;

typedef enum {
    STMT_EXPRESSION,
    STMT_VARIABLE,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_RETURN,
    STMT_FUNCTION
} StmtType;

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct {
    DataType data_type;
    Token name;
} FunctionParameter;

struct Expr {
    ExprType type;

    union {
        struct {
            Token value;
            DataType data_type;
        } literal;

        struct {
            Token name;
        } variable;

        struct {
            Expr *left;
            Token operator;
            Expr *right;
        } binary;

        struct {
            Token operator;
            Expr *right;
        } unary;

        struct {
            Token name;
            Expr *value;
            Expr *target;
        } assign;

        struct {
            Expr *callee;
            Expr **arguments;
            size_t argument_count;
        } call;

        struct {
            Expr *object;
            Token name;
        } member;

        struct {
            Expr *object;
            Expr *index;
        } index;

        struct {
            Expr **elements;
            size_t count;
        } array_literal;

        struct {
            DataType data_type;
            Expr *length;
        } array_create;
    };
};

struct Stmt {
    StmtType type;

    union {
        struct {
            Expr *expression;
        } expression;

        struct {
            DataType data_type;
            Token name;
            Expr *initializer;
        } variable;

        struct {
            Stmt **statements;
            size_t count;
        } block;

        struct {
            Expr *condition;
            Stmt *then_branch;
            Stmt *else_branch;
        } if_stmt;

        struct {
            Expr *condition;
            Stmt *body;
        } while_stmt;

        struct {
            Stmt *initializer;
            Expr *condition;
            Expr *increment;
            Stmt *body;
        } for_stmt;

        struct {
            Token keyword;
            Expr *value;
        } return_stmt;

        struct {
            DataType return_type;
            Token name;
            FunctionParameter *parameters;
            size_t parameter_count;
            Stmt *body;
        } function;
    };
};

void ast_free_expr(Expr *expr);
void ast_free_stmt(Stmt *stmt);
void ast_print_stmt(Stmt *stmt, int depth);
#endif
