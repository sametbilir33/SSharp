
#include <stdio.h>
#include <stdlib.h>

#include "ast.h"

void ast_free_expr(Expr *expr)
{
    if (!expr)
        return;

    switch (expr->type) {
        case EXPR_BINARY:
            ast_free_expr(expr->binary.left);
            ast_free_expr(expr->binary.right);
            break;

        case EXPR_UNARY:
            ast_free_expr(expr->unary.right);
            break;

        case EXPR_ASSIGN:
            ast_free_expr(expr->assign.value);
            break;

        case EXPR_CALL:
            ast_free_expr(expr->call.callee);

            for (size_t i = 0; i < expr->call.argument_count; i++)
                ast_free_expr(expr->call.arguments[i]);

            free(expr->call.arguments);
            break;

        case EXPR_MEMBER:
            ast_free_expr(expr->member.object);
            break;

        case EXPR_LITERAL:
        case EXPR_VARIABLE:
            break;
    }

    free(expr);
}

void ast_free_stmt(Stmt *stmt)
{
    if (!stmt)
        return;

    switch (stmt->type) {
        case STMT_EXPRESSION:
            ast_free_expr(stmt->expression.expression);
            break;

        case STMT_VARIABLE:
            ast_free_expr(stmt->variable.initializer);
            break;

        case STMT_BLOCK:
            for (size_t i = 0; i < stmt->block.count; i++)
                ast_free_stmt(stmt->block.statements[i]);

            free(stmt->block.statements);
            break;

        case STMT_IF:
            ast_free_expr(stmt->if_stmt.condition);
            ast_free_stmt(stmt->if_stmt.then_branch);
            ast_free_stmt(stmt->if_stmt.else_branch);
            break;

        case STMT_WHILE:
            ast_free_expr(stmt->while_stmt.condition);
            ast_free_stmt(stmt->while_stmt.body);
            break;

        case STMT_FOR:
            ast_free_stmt(stmt->for_stmt.initializer);
            ast_free_expr(stmt->for_stmt.condition);
            ast_free_expr(stmt->for_stmt.increment);
            ast_free_stmt(stmt->for_stmt.body);
            break;

        case STMT_RETURN:
            ast_free_expr(stmt->return_stmt.value);
            break;

        case STMT_FUNCTION:
            free(stmt->function.parameters);
            ast_free_stmt(stmt->function.body);
            break;
    }

    free(stmt);
}

static void print_indent(int depth)
{
    for (int i = 0; i < depth; i++)
        printf("  ");
}

static const char *data_type_name(DataType type)
{
    switch (type) {
        case TYPE_INT: return "int";
        case TYPE_FLOAT: return "float";
        case TYPE_DOUBLE: return "double";
        case TYPE_BOOL: return "bool";
        case TYPE_STRING: return "string";
        case TYPE_VOID: return "void";
        case TYPE_NULL: return "null";
        case TYPE_ERROR: return "error";
    }

    return "unknown";
}

static void print_expr(Expr *expr, int depth)
{
    if (!expr)
        return;

    switch (expr->type) {
        case EXPR_LITERAL:
            print_indent(depth);
            printf("LITERAL: %.*s\n",
                (int)expr->literal.value.length,
                expr->literal.value.start);
            break;

        case EXPR_VARIABLE:
            print_indent(depth);
            printf("VARIABLE: %.*s\n",
                (int)expr->variable.name.length,
                expr->variable.name.start);
            break;

        case EXPR_BINARY:
            print_indent(depth);
            printf("BINARY: %.*s\n",
                (int)expr->binary.operator.length,
                expr->binary.operator.start);

            print_expr(expr->binary.left, depth + 1);
            print_expr(expr->binary.right, depth + 1);
            break;

        case EXPR_UNARY:
            print_indent(depth);
            printf("UNARY: %.*s\n",
                (int)expr->unary.operator.length,
                expr->unary.operator.start);

            print_expr(expr->unary.right, depth + 1);
            break;

        case EXPR_ASSIGN:
            print_indent(depth);
            printf("ASSIGN\n");

            print_indent(depth + 1);
            printf("NAME: %.*s\n",
                (int)expr->assign.name.length,
                expr->assign.name.start);

            print_expr(expr->assign.value, depth + 1);
            break;

        case EXPR_CALL:
            print_indent(depth);
            printf("CALL\n");

            print_expr(expr->call.callee, depth + 1);

            for (size_t i = 0; i < expr->call.argument_count; i++)
                print_expr(expr->call.arguments[i], depth + 1);

            break;

        case EXPR_MEMBER:
            print_indent(depth);
            printf("MEMBER: %.*s\n",
                (int)expr->member.name.length,
                expr->member.name.start);

            print_expr(expr->member.object, depth + 1);
            break;
    }
}

void ast_print_stmt(Stmt *stmt, int depth)
{
    if (!stmt)
        return;

    switch (stmt->type) {
        case STMT_EXPRESSION:
            print_indent(depth);
            printf("EXPRESSION_STATEMENT\n");
            print_expr(stmt->expression.expression, depth + 1);
            break;

        case STMT_VARIABLE:
            print_indent(depth);
            printf("VARIABLE_DECLARATION: %.*s\n",
                (int)stmt->variable.name.length,
                stmt->variable.name.start);

            if (stmt->variable.initializer)
                print_expr(stmt->variable.initializer, depth + 1);

            break;

        case STMT_BLOCK:
            print_indent(depth);
            printf("BLOCK\n");

            for (size_t i = 0; i < stmt->block.count; i++)
                ast_print_stmt(stmt->block.statements[i], depth + 1);

            break;

        case STMT_IF:
            print_indent(depth);
            printf("IF_STATEMENT\n");

            print_indent(depth + 1);
            printf("CONDITION\n");

            print_expr(stmt->if_stmt.condition, depth + 2);

            print_indent(depth + 1);
            printf("THEN\n");

            ast_print_stmt(stmt->if_stmt.then_branch, depth + 2);

            if (stmt->if_stmt.else_branch) {
                print_indent(depth + 1);
                printf("ELSE\n");

                ast_print_stmt(stmt->if_stmt.else_branch, depth + 2);
            }

            break;

        case STMT_WHILE:
            print_indent(depth);
            printf("WHILE_STATEMENT\n");

            print_indent(depth + 1);
            printf("CONDITION\n");
            print_expr(stmt->while_stmt.condition, depth + 2);

            print_indent(depth + 1);
            printf("BODY\n");
            ast_print_stmt(stmt->while_stmt.body, depth + 2);
            break;

        case STMT_FOR:
            print_indent(depth);
            printf("FOR_STATEMENT\n");

            if (stmt->for_stmt.initializer) {
                print_indent(depth + 1);
                printf("INITIALIZER\n");
                ast_print_stmt(stmt->for_stmt.initializer, depth + 2);
            }

            if (stmt->for_stmt.condition) {
                print_indent(depth + 1);
                printf("CONDITION\n");
                print_expr(stmt->for_stmt.condition, depth + 2);
            }

            if (stmt->for_stmt.increment) {
                print_indent(depth + 1);
                printf("INCREMENT\n");
                print_expr(stmt->for_stmt.increment, depth + 2);
            }

            print_indent(depth + 1);
            printf("BODY\n");
            ast_print_stmt(stmt->for_stmt.body, depth + 2);
            break;

        case STMT_RETURN:
            print_indent(depth);
            printf("RETURN_STATEMENT\n");
            if (stmt->return_stmt.value)
                print_expr(stmt->return_stmt.value, depth + 1);
            break;

        case STMT_FUNCTION:
            print_indent(depth);
            printf("FUNCTION_DECLARATION: %.*s (return type: %s)\n",
                (int)stmt->function.name.length,
                stmt->function.name.start,
                data_type_name(stmt->function.return_type));

            for (size_t i = 0; i < stmt->function.parameter_count; i++) {
                print_indent(depth + 1);
                printf("PARAMETER: %.*s (type: %s)\n",
                    (int)stmt->function.parameters[i].name.length,
                    stmt->function.parameters[i].name.start,
                    data_type_name(
                        stmt->function.parameters[i].data_type
                    ));
            }

            ast_print_stmt(stmt->function.body, depth + 1);
            break;
    }
}
