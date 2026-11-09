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

static int token_is(Token token, const char *text)
{
    size_t length = strlen(text);
    return token.length == length &&
        memcmp(token.start, text, length) == 0;
}

static int is_console_object(Expr *object)
{
    return object && object->type == EXPR_VARIABLE &&
        token_is(object->variable.name, "Console");
}

static int is_string_type_object(Expr *object)
{
    return object && object->type == EXPR_VARIABLE &&
        token_is(object->variable.name, "String");
}

static int is_convert_type_object(Expr *object)
{
    return object && object->type == EXPR_VARIABLE &&
        token_is(object->variable.name, "Convert");
}

static int is_array_type_object(Expr *object)
{
    return object && object->type == EXPR_VARIABLE &&
        token_is(object->variable.name, "Array");
}

static int is_array_method(Token name)
{
    return token_is(name, "Sort") ||
        token_is(name, "Reverse") ||
        token_is(name, "IndexOf") ||
        token_is(name, "LastIndexOf") ||
        token_is(name, "Copy") ||
        token_is(name, "Resize") ||
        token_is(name, "Clear") ||
        token_is(name, "Fill");
}

static DataType console_property_type(Token name)
{
    if (token_is(name, "Title"))
        return TYPE_STRING;
    if (token_is(name, "ForegroundColor") ||
        token_is(name, "BackgroundColor") ||
        token_is(name, "CursorLeft") ||
        token_is(name, "CursorTop"))
        return TYPE_INT;
    return TYPE_ERROR;
}

static int token_is_console_call(Expr *callee, const char *name)
{
    return callee && callee->type == EXPR_MEMBER &&
        is_console_object(callee->member.object) &&
        token_is(callee->member.name, name);
}

static DataType string_method_return_type(Token name)
{
    if (token_is(name, "Contains") ||
        token_is(name, "StartsWith") ||
        token_is(name, "EndsWith"))
        return TYPE_BOOL;
    if (token_is(name, "IndexOf") ||
        token_is(name, "LastIndexOf"))
        return TYPE_INT;
    if (token_is(name, "Replace") ||
        token_is(name, "Substring") ||
        token_is(name, "Trim") ||
        token_is(name, "TrimStart") ||
        token_is(name, "TrimEnd") ||
        token_is(name, "ToUpper") ||
        token_is(name, "ToLower"))
        return TYPE_STRING;
    return TYPE_ERROR;
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

static int is_array_type(DataType type)
{
    return type == TYPE_INT_ARRAY ||
        type == TYPE_BOOL_ARRAY ||
        type == TYPE_STRING_ARRAY;
}

static DataType array_element_type(DataType type)
{
    if (type == TYPE_INT_ARRAY) return TYPE_INT;
    if (type == TYPE_BOOL_ARRAY) return TYPE_BOOL;
    if (type == TYPE_STRING_ARRAY) return TYPE_STRING;
    return TYPE_ERROR;
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
    if (is_array_type(target) && value == TYPE_NULL)
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
        case EXPR_INDEX:
            return expression_token(expr->index.object);
        case EXPR_ARRAY_LITERAL:
            return (Token){0};
        case EXPR_ARRAY_CREATE:
            return expression_token(expr->array_create.length);
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
        if (is_convert_type_object(callee->member.object)) {
            Token method = callee->member.name;
            DataType result_type =
                token_is(method, "ToByte") ||
                token_is(method, "ToSByte") ||
                token_is(method, "ToInt16") ||
                token_is(method, "ToUInt16") ||
                token_is(method, "ToInt32") ||
                token_is(method, "ToUInt32") ||
                token_is(method, "ToInt64")
                    ? TYPE_INT
                : token_is(method, "ToUInt64") ? TYPE_UINT64
                : token_is(method, "ToSingle") ? TYPE_FLOAT
                : token_is(method, "ToDouble") ||
                  token_is(method, "ToDecimal") ? TYPE_DOUBLE
                : token_is(method, "ToBoolean") ? TYPE_BOOL
                : token_is(method, "ToChar") ? TYPE_CHAR
                : token_is(method, "ToString") ? TYPE_STRING
                : TYPE_ERROR;
            if (result_type == TYPE_ERROR) {
                for (size_t i = 0; i < expr->call.argument_count; i++)
                    analyze_expression(analyzer, expr->call.arguments[i]);
                report_error(analyzer, method, "Unsupported Convert method");
                return TYPE_ERROR;
            }
            if (expr->call.argument_count != 1) {
                report_error(analyzer, method,
                    "Convert methods require exactly one argument");
            }
            for (size_t i = 0; i < expr->call.argument_count; i++) {
                DataType argument_type =
                    analyze_expression(analyzer, expr->call.arguments[i]);
                if (argument_type != TYPE_INT &&
                    argument_type != TYPE_FLOAT &&
                    argument_type != TYPE_DOUBLE &&
                    argument_type != TYPE_UINT64 &&
                    argument_type != TYPE_CHAR &&
                    argument_type != TYPE_BOOL &&
                    argument_type != TYPE_STRING &&
                    argument_type != TYPE_NULL &&
                    argument_type != TYPE_ERROR)
                    report_error(analyzer,
                        expression_token(expr->call.arguments[i]),
                        "Convert argument type is not supported");
            }
            return result_type;
        }

        if (is_array_type_object(callee->member.object)) {
            Token method = callee->member.name;
            if (!is_array_method(method)) {
                for (size_t i = 0; i < expr->call.argument_count; i++)
                    analyze_expression(analyzer, expr->call.arguments[i]);
                report_error(analyzer, method, "Unsupported Array method");
                return TYPE_ERROR;
            }
            if (expr->call.argument_count == 0) {
                report_error(analyzer, method,
                    "Array method requires an array argument");
                return TYPE_ERROR;
            }

            DataType array_type =
                analyze_expression(analyzer, expr->call.arguments[0]);
            if (!is_array_type(array_type) && array_type != TYPE_ERROR)
                report_error(analyzer,
                    expression_token(expr->call.arguments[0]),
                    "Array method requires a one-dimensional array");

            size_t count = expr->call.argument_count;
            int valid_count =
                (token_is(method, "Sort") || token_is(method, "Reverse"))
                    ? count == 1
                : (token_is(method, "IndexOf") ||
                   token_is(method, "LastIndexOf") ||
                   token_is(method, "Resize")) ? count == 2
                : token_is(method, "Copy") ? count == 5
                : token_is(method, "Clear") ? count == 3
                : token_is(method, "Fill") ? count == 2 || count == 4
                : 0;
            if (!valid_count)
                report_error(analyzer, method,
                    "Incorrect number of Array method arguments");

            for (size_t i = 1; i < count; i++) {
                DataType argument_type =
                    analyze_expression(analyzer, expr->call.arguments[i]);
                int integer_argument =
                    token_is(method, "Copy") ||
                    token_is(method, "Resize") ||
                    token_is(method, "Clear") ||
                    (token_is(method, "Fill") && count == 4 &&
                     (i == 2 || i == 3));
                if (token_is(method, "Copy") && i == 2) {
                    if (argument_type != array_type &&
                        argument_type != TYPE_ERROR)
                        report_error(analyzer,
                            expression_token(expr->call.arguments[i]),
                            "Array.Copy source and destination types must match");
                    continue;
                }
                if (integer_argument && argument_type != TYPE_INT &&
                    argument_type != TYPE_ERROR)
                    report_error(analyzer,
                        expression_token(expr->call.arguments[i]),
                        "Array method index, length, and size arguments must be integers");
                else if (!integer_argument &&
                    (token_is(method, "IndexOf") ||
                     token_is(method, "LastIndexOf") ||
                     token_is(method, "Fill")) &&
                    argument_type != array_element_type(array_type) &&
                    !(array_element_type(array_type) == TYPE_STRING &&
                      argument_type == TYPE_NULL) &&
                    argument_type != TYPE_ERROR)
                    report_error(analyzer,
                        expression_token(expr->call.arguments[i]),
                        "Array value type does not match the array element type");
            }

            if (token_is(method, "IndexOf") ||
                token_is(method, "LastIndexOf"))
                return TYPE_INT;
            if (token_is(method, "Resize"))
                return array_type;
            return TYPE_VOID;
        }

        if (is_string_type_object(callee->member.object)) {
            Token method = callee->member.name;
            if (token_is(method, "IsNullOrEmpty") ||
                token_is(method, "IsNullOrWhiteSpace")) {
                if (expr->call.argument_count != 1)
                    report_error(analyzer, method,
                        "String null-check methods require one argument");
                for (size_t i = 0; i < expr->call.argument_count; i++) {
                    DataType argument_type =
                        analyze_expression(analyzer, expr->call.arguments[i]);
                    if (argument_type != TYPE_STRING &&
                        argument_type != TYPE_NULL &&
                        argument_type != TYPE_ERROR)
                        report_error(analyzer,
                            expression_token(expr->call.arguments[i]),
                            "String null-check methods require a string");
                }
                return TYPE_BOOL;
            }
            if (token_is(method, "Format")) {
                if (expr->call.argument_count == 0) {
                    report_error(analyzer, method,
                        "String.Format requires a format string");
                    return TYPE_ERROR;
                }
                DataType format_type =
                    analyze_expression(analyzer, expr->call.arguments[0]);
                if (format_type != TYPE_STRING && format_type != TYPE_ERROR)
                    report_error(analyzer,
                        expression_token(expr->call.arguments[0]),
                        "String.Format format must be a string");
                for (size_t i = 1; i < expr->call.argument_count; i++) {
                    DataType argument_type =
                        analyze_expression(analyzer, expr->call.arguments[i]);
                    if (argument_type != TYPE_STRING &&
                        argument_type != TYPE_INT &&
                        argument_type != TYPE_BOOL &&
                        argument_type != TYPE_NULL &&
                        argument_type != TYPE_ERROR)
                        report_error(analyzer,
                            expression_token(expr->call.arguments[i]),
                            "String.Format values must be strings, integers, or booleans");
                }
                return TYPE_STRING;
            }
            for (size_t i = 0; i < expr->call.argument_count; i++)
                analyze_expression(analyzer, expr->call.arguments[i]);
            report_error(analyzer, method, "Unsupported static String method");
            return TYPE_ERROR;
        }

        if (token_is_console_call(callee, "ReadLine") ||
            token_is_console_call(callee, "ReadKey")) {
            for (size_t i = 0; i < expr->call.argument_count; i++)
                analyze_expression(analyzer, expr->call.arguments[i]);
            if (expr->call.argument_count != 0)
                report_error(analyzer, callee->member.name,
                    "Console input methods accept no arguments");
            return TYPE_STRING;
        }

        if (token_is_console_call(callee, "SetCursorPosition")) {
            if (expr->call.argument_count != 2) {
                report_error(analyzer, callee->member.name,
                    "Console.SetCursorPosition requires two integer arguments");
            } else {
                for (size_t i = 0; i < expr->call.argument_count; i++) {
                    if (analyze_expression(analyzer, expr->call.arguments[i]) !=
                        TYPE_INT) {
                        report_error(analyzer, callee->member.name,
                            "Console.SetCursorPosition arguments must be integers");
                    }
                }
            }
            return TYPE_VOID;
        }

        if (is_console_object(callee->member.object)) {
            for (size_t i = 0; i < expr->call.argument_count; i++)
                analyze_expression(analyzer, expr->call.arguments[i]);
            return TYPE_VOID;
        }

        DataType receiver_type =
            analyze_expression(analyzer, callee->member.object);
        if (receiver_type == TYPE_STRING) {
            DataType result_type =
                string_method_return_type(callee->member.name);
            if (result_type == TYPE_ERROR) {
                for (size_t i = 0; i < expr->call.argument_count; i++)
                    analyze_expression(analyzer, expr->call.arguments[i]);
                report_error(analyzer, callee->member.name,
                    "Unsupported String method");
                return TYPE_ERROR;
            }

            size_t expected_count =
                token_is(callee->member.name, "Replace") ? 2 :
                (token_is(callee->member.name, "Substring") ? 0 :
                (token_is(callee->member.name, "Trim") ||
                 token_is(callee->member.name, "TrimStart") ||
                 token_is(callee->member.name, "TrimEnd") ||
                 token_is(callee->member.name, "ToUpper") ||
                 token_is(callee->member.name, "ToLower") ? 0 : 1));
            int valid_count = token_is(callee->member.name, "Substring")
                ? expr->call.argument_count == 1 ||
                    expr->call.argument_count == 2
                : expr->call.argument_count == expected_count;
            if (!valid_count)
                report_error(analyzer, callee->member.name,
                    "Incorrect number of String method arguments");

            for (size_t i = 0; i < expr->call.argument_count; i++) {
                DataType argument_type =
                    analyze_expression(analyzer, expr->call.arguments[i]);
                DataType expected_type =
                    token_is(callee->member.name, "Substring")
                        ? TYPE_INT
                        : TYPE_STRING;
                if (argument_type != TYPE_ERROR &&
                    argument_type != expected_type)
                    report_error(analyzer,
                        expression_token(expr->call.arguments[i]),
                        token_is(callee->member.name, "Substring")
                            ? "String.Substring arguments must be integers"
                            : "String method arguments must be strings");
            }
            return result_type;
        }

        for (size_t i = 0; i < expr->call.argument_count; i++)
            analyze_expression(analyzer, expr->call.arguments[i]);
        if (receiver_type != TYPE_ERROR)
            report_error(analyzer, callee->member.name,
                "Instance methods are only supported on strings");
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
                ((left == TYPE_STRING &&
                  (right == TYPE_STRING || right == TYPE_NULL)) ||
                 (left == TYPE_NULL && right == TYPE_STRING)))
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
            if (expr->assign.target) {
                Expr *target = expr->assign.target;
                DataType value = analyze_expression(analyzer, expr->assign.value);
                if (target->type == EXPR_INDEX) {
                    DataType target_type = analyze_expression(analyzer, target);
                    if (value != TYPE_ERROR &&
                        !can_assign(target_type, value))
                        report_error(analyzer, expr->assign.name,
                            "Array element assignment type mismatch");
                    return target_type;
                }
                if (target->type != EXPR_MEMBER ||
                    !is_console_object(target->member.object)) {
                    report_error(analyzer, expr->assign.name,
                        "Only Console properties can be assigned through member access");
                    return TYPE_ERROR;
                }

                DataType property_type =
                    console_property_type(target->member.name);
                if (property_type == TYPE_ERROR) {
                    report_error(analyzer, target->member.name,
                        "Unsupported Console property");
                    return TYPE_ERROR;
                }
                if ((token_is(target->member.name, "CursorLeft") ||
                     token_is(target->member.name, "CursorTop")) &&
                    property_type == TYPE_INT) {
                    report_error(analyzer, target->member.name,
                        "Cursor position properties are read-only");
                    return TYPE_ERROR;
                }
                if (value != TYPE_ERROR &&
                    !can_assign(property_type, value)) {
                    report_error(analyzer, target->member.name,
                        "Console property assignment type mismatch");
                    return TYPE_ERROR;
                }
                return property_type;
            }

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
            if (is_console_object(expr->member.object)) {
                DataType property_type =
                    console_property_type(expr->member.name);
                if (property_type == TYPE_ERROR) {
                    report_error(analyzer, expr->member.name,
                        "Unsupported Console property");
                    return TYPE_ERROR;
                }
                report_error(analyzer, expr->member.name,
                    "Console property getters are not supported by this terminal backend");
                return TYPE_ERROR;
            }
            {
                DataType object_type =
                    analyze_expression(analyzer, expr->member.object);
                if ((object_type == TYPE_STRING ||
                     is_array_type(object_type)) &&
                    token_is(expr->member.name, "Length"))
                    return TYPE_INT;
            }
            report_error(
                analyzer,
                expr->member.name,
                "Member access is only supported as a function call"
            );
            return TYPE_ERROR;

        case EXPR_INDEX: {
            DataType object_type =
                analyze_expression(analyzer, expr->index.object);
            DataType index_type =
                analyze_expression(analyzer, expr->index.index);
            DataType element_type = array_element_type(object_type);
            if (element_type == TYPE_ERROR && object_type != TYPE_ERROR)
                report_error(analyzer, expression_token(expr->index.object),
                    "Indexing requires a single-dimensional array");
            if (index_type != TYPE_INT && index_type != TYPE_ERROR)
                report_error(analyzer, expression_token(expr->index.index),
                    "Array index must be an integer");
            return element_type;
        }

        case EXPR_ARRAY_LITERAL:
            report_error(analyzer, (Token){0},
                "Array initializer is only valid in an array declaration");
            return TYPE_ERROR;

        case EXPR_ARRAY_CREATE: {
            DataType length_type =
                analyze_expression(analyzer, expr->array_create.length);
            if (length_type != TYPE_INT && length_type != TYPE_ERROR)
                report_error(analyzer,
                    expression_token(expr->array_create.length),
                    "Array length must be an integer");
            return expr->array_create.data_type;
        }
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
                Expr *initializer_expr = stmt->variable.initializer;
                if (is_array_type(stmt->variable.data_type) &&
                    initializer_expr->type == EXPR_ARRAY_LITERAL) {
                    DataType element_type =
                        array_element_type(stmt->variable.data_type);
                    for (size_t i = 0;
                         i < initializer_expr->array_literal.count; i++) {
                        Expr *item =
                            initializer_expr->array_literal.elements[i];
                        DataType item_type = analyze_expression(analyzer, item);
                        if (item_type != TYPE_ERROR &&
                            !can_assign(element_type, item_type))
                            report_error(analyzer, expression_token(item),
                                "Array initializer element type mismatch");
                    }
                } else {
                    DataType initializer =
                        analyze_expression(analyzer, initializer_expr);
                    if (initializer != TYPE_ERROR &&
                        !can_assign(stmt->variable.data_type, initializer))
                        report_error(analyzer, stmt->variable.name,
                            "Variable initializer type mismatch");
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
