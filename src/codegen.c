#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "elf_writer.h"

typedef struct {
    unsigned char *bytes;
    size_t size;
    size_t capacity;
} Buffer;

typedef struct Binding {
    Token name;
    DataType type;
    size_t slot;
    struct Binding *previous;
} Binding;

typedef struct {
    size_t position;
    int defined;
} Label;

typedef struct {
    size_t displacement_offset;
    size_t label;
    int is_data;
} Fixup;

typedef struct {
    Buffer code;
    Buffer data;
    Label *labels;
    size_t label_count;
    size_t label_capacity;
    Fixup *fixups;
    size_t fixup_count;
    size_t fixup_capacity;
    Binding *bindings;
    Stmt *program;
    size_t next_slot;
    size_t stack_depth;
    size_t *function_labels;
    size_t *function_epilogues;
    size_t statement_count;
    int failed;
} Generator;

enum {
    REG_RAX = 0,
    REG_RCX = 1,
    REG_RDX = 2,
    REG_RBX = 3,
    REG_RSP = 4,
    REG_RBP = 5,
    REG_RSI = 6,
    REG_RDI = 7,
    REG_R8 = 8,
    REG_R9 = 9,
    REG_R10 = 10
};

static void report_error(Generator *generator, Token token, const char *message)
{
    fprintf(stderr,
        "S# Codegen: %s at %d:%d\n",
        message,
        token.line,
        token.column);
    generator->failed = 1;
}

static int reserve(Buffer *buffer, size_t additional)
{
    if (additional > SIZE_MAX - buffer->size)
        return 0;
    size_t needed = buffer->size + additional;
    if (needed <= buffer->capacity)
        return 1;

    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    unsigned char *bytes = realloc(buffer->bytes, capacity);
    if (!bytes)
        return 0;
    buffer->bytes = bytes;
    buffer->capacity = capacity;
    return 1;
}

static void emit8(Generator *generator, unsigned value)
{
    if (generator->failed)
        return;
    if (!reserve(&generator->code, 1)) {
        fprintf(stderr, "S#: Out of memory while emitting machine code.\n");
        generator->failed = 1;
        return;
    }
    generator->code.bytes[generator->code.size++] = (unsigned char)value;
}

static void emit_data8(Generator *generator, unsigned value)
{
    if (generator->failed)
        return;
    if (!reserve(&generator->data, 1)) {
        fprintf(stderr, "S#: Out of memory while emitting data.\n");
        generator->failed = 1;
        return;
    }
    generator->data.bytes[generator->data.size++] = (unsigned char)value;
}

static void emit32(Generator *generator, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++)
        emit8(generator, value >> (i * 8));
}

static void emit64(Generator *generator, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++)
        emit8(generator, (unsigned)(value >> (i * 8)));
}

static void emit_mov_imm64(Generator *generator, unsigned reg, uint64_t value)
{
    emit8(generator, 0x48 | (reg >= 8 ? 0x01 : 0));
    emit8(generator, 0xb8 + (reg & 7));
    emit64(generator, value);
}

static void emit_mov_reg_reg(Generator *generator, unsigned dst, unsigned src)
{
    emit8(generator, 0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0));
    emit8(generator, 0x89);
    emit8(generator, 0xc0 | ((src & 7) << 3) | (dst & 7));
}

static void emit_push(Generator *generator, unsigned reg)
{
    if (reg >= 8)
        emit8(generator, 0x41);
    emit8(generator, 0x50 + (reg & 7));
    generator->stack_depth += 8;
}

static void emit_pop(Generator *generator, unsigned reg)
{
    if (reg >= 8)
        emit8(generator, 0x41);
    emit8(generator, 0x58 + (reg & 7));
    generator->stack_depth -= 8;
}

static void emit_stack_adjust(Generator *generator, int subtract, size_t amount)
{
    if (amount == 0)
        return;
    emit8(generator, 0x48);
    emit8(generator, 0x81);
    emit8(generator, subtract ? 0xec : 0xc4);
    emit32(generator, (uint32_t)amount);
    if (subtract)
        generator->stack_depth += amount;
    else
        generator->stack_depth -= amount;
}

static void emit_load_rbp(Generator *generator, unsigned reg, size_t offset)
{
    emit8(generator, 0x48 | (reg >= 8 ? 0x04 : 0));
    emit8(generator, 0x8b);
    emit8(generator, 0x85 | ((reg & 7) << 3));
    emit32(generator, (uint32_t)-(int32_t)offset);
}

static void emit_store_rbp(Generator *generator, size_t offset, unsigned reg)
{
    emit8(generator, 0x48 | (reg >= 8 ? 4 : 0));
    emit8(generator, 0x89);
    emit8(generator, 0x85 | ((reg & 7) << 3));
    emit32(generator, (uint32_t)-(int32_t)offset);
}

static void emit_load_rsp(Generator *generator, unsigned reg, size_t offset)
{
    emit8(generator, 0x48 | (reg >= 8 ? 0x04 : 0));
    emit8(generator, 0x8b);
    emit8(generator, 0x84 | ((reg & 7) << 3));
    emit8(generator, 0x24);
    emit32(generator, (uint32_t)offset);
}

static void emit_test_rax(Generator *generator)
{
    emit8(generator, 0x48);
    emit8(generator, 0x85);
    emit8(generator, 0xc0);
}

static size_t new_label(Generator *generator)
{
    if (generator->label_count == generator->label_capacity) {
        size_t capacity = generator->label_capacity
            ? generator->label_capacity * 2
            : 32;
        Label *labels = realloc(
            generator->labels,
            capacity * sizeof(Label)
        );
        if (!labels) {
            fprintf(stderr, "S#: Out of memory while allocating labels.\n");
            generator->failed = 1;
            return 0;
        }
        generator->labels = labels;
        generator->label_capacity = capacity;
    }

    size_t label = generator->label_count++;
    generator->labels[label] = (Label){0};
    return label;
}

static void define_label(Generator *generator, size_t label)
{
    if (label >= generator->label_count) {
        generator->failed = 1;
        return;
    }
    generator->labels[label].position = generator->code.size;
    generator->labels[label].defined = 1;
}

static void add_fixup(Generator *generator, size_t label, int is_data)
{
    if (generator->fixup_count == generator->fixup_capacity) {
        size_t capacity = generator->fixup_capacity
            ? generator->fixup_capacity * 2
            : 32;
        Fixup *fixups = realloc(
            generator->fixups,
            capacity * sizeof(Fixup)
        );
        if (!fixups) {
            fprintf(stderr, "S#: Out of memory while allocating fixups.\n");
            generator->failed = 1;
            return;
        }
        generator->fixups = fixups;
        generator->fixup_capacity = capacity;
    }

    generator->fixups[generator->fixup_count++] = (Fixup){
        generator->code.size,
        label,
        is_data
    };
    emit32(generator, 0);
}

static void emit_jump(Generator *generator, size_t label)
{
    emit8(generator, 0xe9);
    add_fixup(generator, label, 0);
}

static void emit_conditional_jump(
    Generator *generator,
    unsigned condition,
    size_t label)
{
    emit8(generator, 0x0f);
    emit8(generator, condition);
    add_fixup(generator, label, 0);
}

static void emit_call_label(Generator *generator, size_t label)
{
    emit8(generator, 0xe8);
    add_fixup(generator, label, 0);
}

static void emit_lea_rax_data(Generator *generator, size_t data_offset)
{
    emit8(generator, 0x48);
    emit8(generator, 0x8d);
    emit8(generator, 0x05);
    add_fixup(generator, data_offset, 1);
}

static void emit_lea_rdi_data(Generator *generator, size_t data_offset)
{
    emit8(generator, 0x48);
    emit8(generator, 0x8d);
    emit8(generator, 0x3d);
    add_fixup(generator, data_offset, 1);
}

static void emit_syscall(Generator *generator)
{
    emit8(generator, 0x0f);
    emit8(generator, 0x05);
}

static void emit_write_syscall(Generator *generator, unsigned fd)
{
    emit_mov_imm64(generator, REG_RAX, 1);
    emit_mov_imm64(generator, REG_RDI, fd);
    emit_syscall(generator);
}

static size_t count_statement_slots(Stmt *stmt);

static size_t count_block_slots(Stmt *block)
{
    size_t count = 0;
    for (size_t i = 0; i < block->block.count; i++)
        count += count_statement_slots(block->block.statements[i]);
    return count;
}

static size_t count_statement_slots(Stmt *stmt)
{
    if (!stmt)
        return 0;
    switch (stmt->type) {
        case STMT_VARIABLE:
            return 1;
        case STMT_BLOCK:
            return count_block_slots(stmt);
        case STMT_IF:
            return count_statement_slots(stmt->if_stmt.then_branch) +
                count_statement_slots(stmt->if_stmt.else_branch);
        case STMT_WHILE:
            return count_statement_slots(stmt->while_stmt.body);
        case STMT_FOR:
            return count_statement_slots(stmt->for_stmt.initializer) +
                count_statement_slots(stmt->for_stmt.body);
        case STMT_EXPRESSION:
        case STMT_RETURN:
        case STMT_FUNCTION:
            return 0;
    }
    return 0;
}

static Binding *find_binding(Generator *generator, Token name)
{
    for (Binding *binding = generator->bindings; binding;
         binding = binding->previous) {
        if (binding->name.length == name.length &&
            memcmp(binding->name.start, name.start, name.length) == 0)
            return binding;
    }
    return NULL;
}

static void add_binding(
    Generator *generator,
    Token name,
    DataType type,
    size_t slot)
{
    Binding *binding = malloc(sizeof(Binding));
    if (!binding) {
        fprintf(stderr, "S#: Out of memory while binding variable.\n");
        generator->failed = 1;
        return;
    }
    binding->name = name;
    binding->type = type;
    binding->slot = slot;
    binding->previous = generator->bindings;
    generator->bindings = binding;
}

static void pop_bindings(Generator *generator, Binding *mark)
{
    while (generator->bindings != mark) {
        Binding *binding = generator->bindings;
        generator->bindings = binding->previous;
        free(binding);
    }
}

static Stmt *find_function(Generator *generator, Token name, size_t *index)
{
    for (size_t i = 0; i < generator->statement_count; i++) {
        Stmt *stmt = generator->program->block.statements[i];
        if (stmt->type == STMT_FUNCTION &&
            stmt->function.name.length == name.length &&
            memcmp(stmt->function.name.start, name.start, name.length) == 0) {
            if (index)
                *index = i;
            return stmt;
        }
    }
    return NULL;
}

static DataType expression_type(Generator *generator, Expr *expr)
{
    if (!expr)
        return TYPE_VOID;

    switch (expr->type) {
        case EXPR_LITERAL:
            return expr->literal.data_type;
        case EXPR_VARIABLE: {
            Binding *binding = find_binding(generator, expr->variable.name);
            return binding ? binding->type : TYPE_ERROR;
        }
        case EXPR_BINARY:
            if (expr->binary.operator.type == TOK_EQUAL_EQUAL ||
                expr->binary.operator.type == TOK_BANG_EQUAL ||
                expr->binary.operator.type == TOK_LESS ||
                expr->binary.operator.type == TOK_LESS_EQUAL ||
                expr->binary.operator.type == TOK_GREATER ||
                expr->binary.operator.type == TOK_GREATER_EQUAL ||
                expr->binary.operator.type == TOK_AND_AND ||
                expr->binary.operator.type == TOK_OR_OR)
                return TYPE_BOOL;
            if (expr->binary.operator.type == TOK_PLUS &&
                expression_type(generator, expr->binary.left) == TYPE_STRING)
                return TYPE_STRING;
            return expression_type(generator, expr->binary.left);
        case EXPR_UNARY:
            return expr->unary.operator.type == TOK_BANG
                ? TYPE_BOOL
                : expression_type(generator, expr->unary.right);
        case EXPR_ASSIGN: {
            Binding *binding = find_binding(generator, expr->assign.name);
            return binding ? binding->type : TYPE_ERROR;
        }
        case EXPR_CALL: {
            if (expr->call.callee &&
                expr->call.callee->type == EXPR_VARIABLE) {
                Stmt *function = find_function(
                    generator,
                    expr->call.callee->variable.name,
                    NULL
                );
                return function ? function->function.return_type : TYPE_ERROR;
            }
            return TYPE_VOID;
        }
        case EXPR_MEMBER:
            return TYPE_ERROR;
    }
    return TYPE_ERROR;
}

static int supported_native_type(
    Generator *generator,
    DataType type,
    Token location)
{
    if (type == TYPE_INT || type == TYPE_BOOL ||
        type == TYPE_STRING || type == TYPE_NULL ||
        type == TYPE_VOID || type == TYPE_ERROR)
        return 1;

    report_error(
        generator,
        location,
        "Direct ELF backend currently supports int, bool, string, and void"
    );
    return 0;
}

static size_t append_string_data(Generator *generator, Token token)
{
    size_t start = generator->data.size;
    size_t prefix = token.type == TOK_INTERPOLATED_STRING ? 2 : 1;
    size_t end = token.length > 0 ? token.length - 1 : 0;

    for (size_t i = prefix; i < end && !generator->failed; i++) {
        unsigned char value = (unsigned char)token.start[i];
        if (value == '\\' && i + 1 < end) {
            unsigned char escaped = (unsigned char)token.start[++i];
            switch (escaped) {
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case '\\': value = '\\'; break;
                case '"': value = '"'; break;
                default: value = escaped; break;
            }
        }
        emit_data8(generator, value);
    }
    emit_data8(generator, 0);
    return start;
}

static size_t append_static_string(Generator *generator, const char *text)
{
    size_t offset = generator->data.size;
    while (*text)
        emit_data8(generator, (unsigned char)*text++);
    emit_data8(generator, 0);
    return offset;
}

static void emit_expression(Generator *generator, Expr *expr);
static void emit_statement(Generator *generator, Stmt *stmt);

static void emit_call_aligned(Generator *generator, size_t label)
{
    size_t padding = generator->stack_depth % 16 == 0 ? 0 : 8;
    if (padding)
        emit_stack_adjust(generator, 1, padding);
    emit_call_label(generator, label);
    if (padding)
        emit_stack_adjust(generator, 0, padding);
}

static void emit_print_string(Generator *generator, Expr *argument)
{
    DataType type = argument ? expression_type(generator, argument) : TYPE_STRING;
    if (!supported_native_type(
            generator,
            type,
            argument ? (argument->type == EXPR_LITERAL
                ? argument->literal.value
                : argument->type == EXPR_VARIABLE
                    ? argument->variable.name
                    : (Token){0})
                : (Token){0}))
        return;

    if (type == TYPE_VOID || type == TYPE_ERROR) {
        report_error(generator, (Token){0},
            "Console.WriteLine argument has no printable value");
        return;
    }

    if (argument) {
        emit_expression(generator, argument);
    } else {
        emit_mov_imm64(generator, REG_RAX, 0);
    }
    emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 1]);
}

static void emit_write_line(Generator *generator, Expr *call)
{
    if (call->call.argument_count > 1) {
        report_error(generator, call->call.callee->member.name,
            "Console.WriteLine accepts zero or one argument");
        return;
    }

    Expr *argument = call->call.argument_count
        ? call->call.arguments[0]
        : NULL;
    DataType type = argument
        ? expression_type(generator, argument)
        : TYPE_STRING;

    if (type == TYPE_STRING || type == TYPE_NULL || !argument) {
        emit_print_string(generator, argument);
        return;
    }

    if (!supported_native_type(
            generator,
            type,
            argument->type == EXPR_LITERAL
                ? argument->literal.value
                : argument->type == EXPR_VARIABLE
                    ? argument->variable.name
                    : (Token){0}))
        return;

    if (type == TYPE_BOOL) {
        size_t false_label = new_label(generator);
        size_t done_label = new_label(generator);
        emit_expression(generator, argument);
        emit_test_rax(generator);
        emit_conditional_jump(generator, 0x84, false_label);
        size_t true_data = append_static_string(generator, "true");
        emit_lea_rdi_data(generator, true_data);
        emit_jump(generator, done_label);
        define_label(generator, false_label);
        size_t false_data = append_static_string(generator, "false");
        emit_lea_rdi_data(generator, false_data);
        define_label(generator, done_label);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count + 1]);
        return;
    }

    if (type == TYPE_INT) {
        emit_expression(generator, argument);
        emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count]);
        return;
    }

    report_error(generator, (Token){0},
        "Unsupported Console.WriteLine argument type");
}

static void emit_function_call(Generator *generator, Expr *call)
{
    Expr *callee = call->call.callee;
    size_t function_index = 0;
    if (!callee || callee->type != EXPR_VARIABLE) {
        report_error(generator, (Token){0}, "Unsupported function call");
        return;
    }

    Stmt *function = find_function(
        generator,
        callee->variable.name,
        &function_index
    );
    if (!function) {
        report_error(generator, callee->variable.name, "Function not found");
        return;
    }
    if (call->call.argument_count != function->function.parameter_count) {
        report_error(generator, callee->variable.name,
            "Incorrect number of function arguments");
        return;
    }
    if (call->call.argument_count > 6) {
        report_error(generator, callee->variable.name,
            "Direct ELF backend currently supports at most six arguments");
        return;
    }

    for (size_t i = 0; i < call->call.argument_count; i++) {
        emit_expression(generator, call->call.arguments[i]);
        emit_push(generator, REG_RAX);
    }

    size_t padding = generator->stack_depth % 16 == 0 ? 0 : 8;
    if (padding)
        emit_stack_adjust(generator, 1, padding);

    static const unsigned registers[] = {
        REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9
    };
    for (size_t i = 0; i < call->call.argument_count; i++) {
        size_t offset = padding + (call->call.argument_count - 1 - i) * 8;
        emit_load_rsp(generator, registers[i], offset);
    }

    emit_call_label(generator, generator->function_labels[function_index]);

    if (padding)
        emit_stack_adjust(generator, 0, padding);
    if (call->call.argument_count)
        emit_stack_adjust(generator, 0, call->call.argument_count * 8);
}

static void emit_binary(Generator *generator, Expr *expr)
{
    TokenType op = expr->binary.operator.type;
    if (op == TOK_PLUS &&
        (expression_type(generator, expr->binary.left) == TYPE_STRING ||
         expression_type(generator, expr->binary.right) == TYPE_STRING)) {
        report_error(
            generator,
            expr->binary.operator,
            "String concatenation is not supported by the direct ELF backend"
        );
        return;
    }

    if (op == TOK_AND_AND || op == TOK_OR_OR) {
        size_t short_label = new_label(generator);
        size_t end_label = new_label(generator);
        emit_expression(generator, expr->binary.left);
        emit_test_rax(generator);
        emit_conditional_jump(
            generator,
            op == TOK_AND_AND ? 0x84 : 0x85,
            short_label
        );
        emit_expression(generator, expr->binary.right);
        emit_test_rax(generator);
        emit8(generator, 0x0f);
        emit8(generator, 0x95);
        emit8(generator, 0xc0);
        emit8(generator, 0x48);
        emit8(generator, 0x0f);
        emit8(generator, 0xb6);
        emit8(generator, 0xc0);
        emit_jump(generator, end_label);
        define_label(generator, short_label);
        emit_mov_imm64(
            generator,
            REG_RAX,
            op == TOK_OR_OR ? 1 : 0
        );
        define_label(generator, end_label);
        return;
    }

    emit_expression(generator, expr->binary.left);
    emit_push(generator, REG_RAX);
    emit_expression(generator, expr->binary.right);
    emit_mov_reg_reg(generator, REG_RCX, REG_RAX);
    emit_pop(generator, REG_RAX);

    DataType left_type = expression_type(generator, expr->binary.left);
    DataType right_type = expression_type(generator, expr->binary.right);
    if ((op == TOK_EQUAL_EQUAL || op == TOK_BANG_EQUAL ||
         op == TOK_LESS || op == TOK_LESS_EQUAL ||
         op == TOK_GREATER || op == TOK_GREATER_EQUAL) &&
        ((left_type == TYPE_STRING && right_type == TYPE_STRING) ||
         (left_type == TYPE_NULL && right_type == TYPE_STRING) ||
         (left_type == TYPE_STRING && right_type == TYPE_NULL))) {
        emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
        emit_mov_reg_reg(generator, REG_RSI, REG_RCX);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count + 2]);
        emit8(generator, 0x48); emit8(generator, 0x83);
        emit8(generator, 0xf8); emit8(generator, 0);

        unsigned condition;
        switch (op) {
            case TOK_EQUAL_EQUAL: condition = 0x94; break;
            case TOK_BANG_EQUAL: condition = 0x95; break;
            case TOK_LESS: condition = 0x9c; break;
            case TOK_LESS_EQUAL: condition = 0x9e; break;
            case TOK_GREATER: condition = 0x9f; break;
            case TOK_GREATER_EQUAL: condition = 0x9d; break;
            default: condition = 0x94; break;
        }
        emit8(generator, 0x0f);
        emit8(generator, condition);
        emit8(generator, 0xc0);
        emit8(generator, 0x48); emit8(generator, 0x0f);
        emit8(generator, 0xb6); emit8(generator, 0xc0);
        return;
    }

    if (op == TOK_PLUS) {
        emit8(generator, 0x48); emit8(generator, 0x01); emit8(generator, 0xc8);
    } else if (op == TOK_MINUS) {
        emit8(generator, 0x48); emit8(generator, 0x29); emit8(generator, 0xc8);
    } else if (op == TOK_STAR) {
        emit8(generator, 0x48); emit8(generator, 0x0f);
        emit8(generator, 0xaf); emit8(generator, 0xc1);
    } else if (op == TOK_SLASH || op == TOK_PERCENT) {
        emit8(generator, 0x48); emit8(generator, 0x99);
        emit8(generator, 0x48); emit8(generator, 0xf7); emit8(generator, 0xf9);
        if (op == TOK_PERCENT)
            emit_mov_reg_reg(generator, REG_RAX, REG_RDX);
    } else {
        emit8(generator, 0x48); emit8(generator, 0x39); emit8(generator, 0xc8);
        unsigned condition;
        switch (op) {
            case TOK_EQUAL_EQUAL: condition = 0x94; break;
            case TOK_BANG_EQUAL: condition = 0x95; break;
            case TOK_LESS: condition = 0x9c; break;
            case TOK_LESS_EQUAL: condition = 0x9e; break;
            case TOK_GREATER: condition = 0x9f; break;
            case TOK_GREATER_EQUAL: condition = 0x9d; break;
            default:
                report_error(generator, expr->binary.operator,
                    "Unsupported binary operator");
                return;
        }
        emit8(generator, 0x0f);
        emit8(generator, condition);
        emit8(generator, 0xc0);
        emit8(generator, 0x48);
        emit8(generator, 0x0f);
        emit8(generator, 0xb6);
        emit8(generator, 0xc0);
    }
}

static void emit_expression(Generator *generator, Expr *expr)
{
    if (!expr || generator->failed)
        return;

    switch (expr->type) {
        case EXPR_LITERAL:
            if (expr->literal.value.type == TOK_NUMBER) {
                char *text = malloc(expr->literal.value.length + 1);
                if (!text) {
                    fprintf(stderr, "S#: Out of memory parsing integer.\n");
                    generator->failed = 1;
                    return;
                }
                memcpy(text, expr->literal.value.start,
                    expr->literal.value.length);
                text[expr->literal.value.length] = '\0';
                errno = 0;
                char *end = NULL;
                long long value = strtoll(text, &end, 10);
                int invalid = errno == ERANGE || end == text || *end != '\0';
                free(text);
                if (invalid) {
                    report_error(generator, expr->literal.value,
                        "Integer literal is outside the 64-bit range");
                    return;
                }
                if (expr->literal.data_type != TYPE_INT) {
                    report_error(generator, expr->literal.value,
                        "Floating-point literals are not supported by the direct ELF backend");
                    return;
                }
                emit_mov_imm64(generator, REG_RAX, (uint64_t)value);
            } else if (expr->literal.value.type == TOK_TRUE) {
                emit_mov_imm64(generator, REG_RAX, 1);
            } else if (expr->literal.value.type == TOK_FALSE ||
                       expr->literal.value.type == TOK_NULL) {
                emit_mov_imm64(generator, REG_RAX, 0);
            } else if (expr->literal.value.type == TOK_STRING) {
                size_t data_offset =
                    append_string_data(generator, expr->literal.value);
                emit_lea_rax_data(generator, data_offset);
            } else {
                report_error(generator, expr->literal.value,
                    "Interpolated strings are not supported by the direct ELF backend");
            }
            break;

        case EXPR_VARIABLE: {
            Binding *binding = find_binding(generator, expr->variable.name);
            if (!binding) {
                report_error(generator, expr->variable.name,
                    "Variable has no native stack slot");
                return;
            }
            emit_load_rbp(generator, REG_RAX, (binding->slot + 1) * 8);
            break;
        }

        case EXPR_BINARY:
            emit_binary(generator, expr);
            break;

        case EXPR_UNARY:
            emit_expression(generator, expr->unary.right);
            if (expr->unary.operator.type == TOK_MINUS) {
                emit8(generator, 0x48); emit8(generator, 0xf7); emit8(generator, 0xd8);
            } else if (expr->unary.operator.type == TOK_BANG) {
                emit_test_rax(generator);
                emit8(generator, 0x0f); emit8(generator, 0x94); emit8(generator, 0xc0);
                emit8(generator, 0x48); emit8(generator, 0x0f);
                emit8(generator, 0xb6); emit8(generator, 0xc0);
            }
            break;

        case EXPR_ASSIGN: {
            Binding *binding = find_binding(generator, expr->assign.name);
            if (!binding) {
                report_error(generator, expr->assign.name,
                    "Assignment target has no native stack slot");
                return;
            }
            emit_expression(generator, expr->assign.value);
            emit_store_rbp(generator, (binding->slot + 1) * 8, REG_RAX);
            break;
        }

        case EXPR_CALL:
            if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                expr->call.callee->member.object &&
                expr->call.callee->member.object->type == EXPR_VARIABLE &&
                expr->call.callee->member.object->variable.name.length == 7 &&
                memcmp(
                    expr->call.callee->member.object->variable.name.start,
                    "Console",
                    7
                ) == 0 &&
                expr->call.callee->member.name.length == 9 &&
                memcmp(
                    expr->call.callee->member.name.start,
                    "WriteLine",
                    9
                ) == 0)
                emit_write_line(generator, expr);
            else
                emit_function_call(generator, expr);
            break;

        case EXPR_MEMBER:
            report_error(generator, expr->member.name,
                "Unsupported member expression");
            break;
    }
}

static void emit_statement(Generator *generator, Stmt *stmt)
{
    if (!stmt || generator->failed)
        return;

    switch (stmt->type) {
        case STMT_EXPRESSION:
            emit_expression(generator, stmt->expression.expression);
            break;

        case STMT_VARIABLE: {
            if (!supported_native_type(
                    generator,
                    stmt->variable.data_type,
                    stmt->variable.name))
                return;
            size_t slot = generator->next_slot++;
            add_binding(
                generator,
                stmt->variable.name,
                stmt->variable.data_type,
                slot
            );
            if (stmt->variable.initializer)
                emit_expression(generator, stmt->variable.initializer);
            else
                emit_mov_imm64(generator, REG_RAX, 0);
            emit_store_rbp(generator, (slot + 1) * 8, REG_RAX);
            break;
        }

        case STMT_BLOCK: {
            Binding *mark = generator->bindings;
            for (size_t i = 0; i < stmt->block.count && !generator->failed; i++)
                emit_statement(generator, stmt->block.statements[i]);
            pop_bindings(generator, mark);
            break;
        }

        case STMT_IF: {
            size_t else_label = new_label(generator);
            size_t end_label = new_label(generator);
            emit_expression(generator, stmt->if_stmt.condition);
            emit_test_rax(generator);
            emit_conditional_jump(generator, 0x84, else_label);
            emit_statement(generator, stmt->if_stmt.then_branch);
            emit_jump(generator, end_label);
            define_label(generator, else_label);
            if (stmt->if_stmt.else_branch)
                emit_statement(generator, stmt->if_stmt.else_branch);
            define_label(generator, end_label);
            break;
        }

        case STMT_WHILE: {
            size_t start_label = new_label(generator);
            size_t end_label = new_label(generator);
            define_label(generator, start_label);
            emit_expression(generator, stmt->while_stmt.condition);
            emit_test_rax(generator);
            emit_conditional_jump(generator, 0x84, end_label);
            emit_statement(generator, stmt->while_stmt.body);
            emit_jump(generator, start_label);
            define_label(generator, end_label);
            break;
        }

        case STMT_FOR: {
            Binding *mark = generator->bindings;
            if (stmt->for_stmt.initializer)
                emit_statement(generator, stmt->for_stmt.initializer);
            size_t start_label = new_label(generator);
            size_t end_label = new_label(generator);
            define_label(generator, start_label);
            if (stmt->for_stmt.condition) {
                emit_expression(generator, stmt->for_stmt.condition);
                emit_test_rax(generator);
                emit_conditional_jump(generator, 0x84, end_label);
            }
            emit_statement(generator, stmt->for_stmt.body);
            if (stmt->for_stmt.increment)
                emit_expression(generator, stmt->for_stmt.increment);
            emit_jump(generator, start_label);
            define_label(generator, end_label);
            pop_bindings(generator, mark);
            break;
        }

        case STMT_RETURN:
            if (stmt->return_stmt.value)
                emit_expression(generator, stmt->return_stmt.value);
            else
                emit_mov_imm64(generator, REG_RAX, 0);
            emit_jump(generator,
                generator->function_epilogues[generator->statement_count]);
            break;

        case STMT_FUNCTION:
            break;
    }
}

static size_t aligned_frame(size_t slots, int entry_function)
{
    size_t bytes = slots * 8;
    size_t frame = (bytes + 15) & ~(size_t)15;
    if (entry_function)
        frame += 8;
    return frame;
}

static void emit_function(Generator *generator, Stmt *function, size_t index)
{
    Token function_name = function->function.name;
    if (function->function.parameter_count > 6) {
        report_error(generator, function_name,
            "Direct ELF backend currently supports at most six parameters");
        return;
    }

    if (!supported_native_type(
            generator,
            function->function.return_type,
            function_name))
        return;
    for (size_t i = 0; i < function->function.parameter_count; i++) {
        if (!supported_native_type(
                generator,
                function->function.parameters[i].data_type,
                function->function.parameters[i].name))
            return;
    }

    Binding *previous_bindings = generator->bindings;
    size_t previous_next_slot = generator->next_slot;
    size_t previous_stack_depth = generator->stack_depth;
    size_t previous_epilogue = generator->function_epilogues[
        generator->statement_count];

    generator->bindings = NULL;
    generator->next_slot = 0;
    generator->stack_depth = 0;
    generator->function_epilogues[generator->statement_count] =
        new_label(generator);

    define_label(generator, generator->function_labels[index]);
    emit8(generator, 0x55);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xe5);

    size_t slots = function->function.parameter_count +
        count_statement_slots(function->function.body);
    size_t frame = aligned_frame(slots, 0);
    if (frame) {
        emit8(generator, 0x48); emit8(generator, 0x81); emit8(generator, 0xec);
        emit32(generator, (uint32_t)frame);
    }

    static const unsigned registers[] = {
        REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9
    };
    for (size_t i = 0; i < function->function.parameter_count; i++) {
        size_t slot = generator->next_slot++;
        add_binding(
            generator,
            function->function.parameters[i].name,
            function->function.parameters[i].data_type,
            slot
        );
        emit_store_rbp(generator, (slot + 1) * 8, registers[i]);
    }

    emit_statement(generator, function->function.body);
    emit_mov_imm64(generator, REG_RAX, 0);
    define_label(
        generator,
        generator->function_epilogues[generator->statement_count]
    );
    emit8(generator, 0xc9);
    emit8(generator, 0xc3);

    pop_bindings(generator, NULL);
    generator->bindings = previous_bindings;
    generator->next_slot = previous_next_slot;
    generator->stack_depth = previous_stack_depth;
    generator->function_epilogues[generator->statement_count] =
        previous_epilogue;
}

static void emit_print_integer_helper(Generator *generator, size_t label)
{
    define_label(generator, label);
    emit8(generator, 0x55);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xe5);
    emit8(generator, 0x48); emit8(generator, 0x83); emit8(generator, 0xec);
    emit8(generator, 0x40);

    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xf8);
    emit8(generator, 0x4d); emit8(generator, 0x31); emit8(generator, 0xc0);
    emit_test_rax(generator);
    size_t positive = new_label(generator);
    size_t digits = new_label(generator);
    emit_conditional_jump(generator, 0x89, positive);
    emit8(generator, 0x48); emit8(generator, 0xf7); emit8(generator, 0xd8);
    emit8(generator, 0x49); emit8(generator, 0xc7); emit8(generator, 0xc0);
    emit32(generator, 1);
    define_label(generator, positive);

    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xee);
    emit8(generator, 0x48); emit8(generator, 0x83); emit8(generator, 0xee);
    emit8(generator, 1);
    emit8(generator, 0xc6); emit8(generator, 0x06); emit8(generator, '\n');
    emit8(generator, 0x49); emit8(generator, 0xc7); emit8(generator, 0xc1);
    emit32(generator, 1);
    define_label(generator, digits);

    size_t digit_loop = new_label(generator);
    define_label(generator, digit_loop);
    emit8(generator, 0x48); emit8(generator, 0x31); emit8(generator, 0xd2);
    emit8(generator, 0x48); emit8(generator, 0xc7); emit8(generator, 0xc1);
    emit32(generator, 10);
    emit8(generator, 0x48); emit8(generator, 0xf7); emit8(generator, 0xf1);
    emit8(generator, 0x80); emit8(generator, 0xc2); emit8(generator, '0');
    emit8(generator, 0x48); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x88); emit8(generator, 0x16);
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xc1);
    emit_test_rax(generator);
    emit_conditional_jump(generator, 0x85, digit_loop);

    emit8(generator, 0x4d); emit8(generator, 0x85); emit8(generator, 0xc0);
    size_t no_sign = new_label(generator);
    emit_conditional_jump(generator, 0x84, no_sign);
    emit8(generator, 0x48); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0xc6); emit8(generator, 0x06); emit8(generator, '-');
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xc1);
    define_label(generator, no_sign);

    emit_mov_reg_reg(generator, REG_RDX, REG_R9);
    emit_write_syscall(generator, 1);
    emit8(generator, 0xc9);
    emit8(generator, 0xc3);
}

static void emit_print_string_helper(Generator *generator, size_t label)
{
    define_label(generator, label);
    emit8(generator, 0x55);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xe5);
    emit8(generator, 0x48); emit8(generator, 0x83); emit8(generator, 0xec);
    emit8(generator, 0x10);
    emit_store_rbp(generator, 8, REG_RDI);
    emit8(generator, 0x48); emit8(generator, 0x85); emit8(generator, 0xff);

    size_t not_null = new_label(generator);
    size_t loop = new_label(generator);
    size_t loop_done = new_label(generator);
    emit_conditional_jump(generator, 0x85, not_null);
    size_t null_data = append_static_string(generator, "null");
    emit_lea_rdi_data(generator, null_data);
    emit_store_rbp(generator, 8, REG_RDI);
    define_label(generator, not_null);

    emit8(generator, 0x48); emit8(generator, 0x31); emit8(generator, 0xc9);
    define_label(generator, loop);
    emit_load_rbp(generator, REG_RDX, 8);
    emit8(generator, 0x8a); emit8(generator, 0x04); emit8(generator, 0x0a);
    emit8(generator, 0x84); emit8(generator, 0xc0);
    emit_conditional_jump(generator, 0x84, loop_done);
    emit8(generator, 0x48); emit8(generator, 0xff); emit8(generator, 0xc1);
    emit_jump(generator, loop);
    define_label(generator, loop_done);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xca);
    emit_load_rbp(generator, REG_RSI, 8);
    emit_write_syscall(generator, 1);

    emit8(generator, 0xc6); emit8(generator, 0x45); emit8(generator, 0xff);
    emit8(generator, '\n');
    emit8(generator, 0x48); emit8(generator, 0x8d); emit8(generator, 0x75);
    emit8(generator, 0xff);
    emit_mov_imm64(generator, REG_RDX, 1);
    emit_write_syscall(generator, 1);
    emit8(generator, 0xc9);
    emit8(generator, 0xc3);
}

static void emit_string_compare_helper(Generator *generator, size_t label)
{
    define_label(generator, label);
    emit8(generator, 0x55);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xe5);
    emit8(generator, 0x48); emit8(generator, 0x83); emit8(generator, 0xec);
    emit8(generator, 0x10);
    emit_store_rbp(generator, 8, REG_RDI);
    emit_store_rbp(generator, 16, REG_RSI);

    size_t left_present = new_label(generator);
    size_t right_present = new_label(generator);
    size_t loop = new_label(generator);
    size_t bytes_differ = new_label(generator);
    size_t equal = new_label(generator);
    size_t done = new_label(generator);

    emit8(generator, 0x48); emit8(generator, 0x85); emit8(generator, 0xff);
    emit_conditional_jump(generator, 0x85, left_present);
    emit8(generator, 0x48); emit8(generator, 0x85); emit8(generator, 0xf6);
    emit_conditional_jump(generator, 0x85, right_present);
    emit_jump(generator, equal);

    define_label(generator, right_present);
    emit8(generator, 0xb8); emit32(generator, (uint32_t)-1);
    emit_jump(generator, done);

    define_label(generator, left_present);
    emit8(generator, 0x48); emit8(generator, 0x85); emit8(generator, 0xf6);
    size_t both_present = new_label(generator);
    emit_conditional_jump(generator, 0x85, both_present);
    emit8(generator, 0xb8); emit32(generator, 1);
    emit_jump(generator, done);

    define_label(generator, both_present);
    define_label(generator, loop);
    emit_load_rbp(generator, REG_RDX, 8);
    emit8(generator, 0x0f); emit8(generator, 0xb6);
    emit8(generator, 0x02);
    emit_load_rbp(generator, REG_RDX, 16);
    emit8(generator, 0x0f); emit8(generator, 0xb6);
    emit8(generator, 0x0a);
    emit8(generator, 0x39); emit8(generator, 0xc8);
    emit_conditional_jump(generator, 0x85, bytes_differ);
    emit8(generator, 0x85); emit8(generator, 0xc0);
    emit_conditional_jump(generator, 0x84, equal);
    emit8(generator, 0x48); emit8(generator, 0x83);
    emit8(generator, 0x45); emit8(generator, 0xf8); emit8(generator, 1);
    emit8(generator, 0x48); emit8(generator, 0x83);
    emit8(generator, 0x45); emit8(generator, 0xf0); emit8(generator, 1);
    emit_jump(generator, loop);

    define_label(generator, bytes_differ);
    emit8(generator, 0x29); emit8(generator, 0xc8);
    emit_jump(generator, done);
    define_label(generator, equal);
    emit8(generator, 0x31); emit8(generator, 0xc0);
    define_label(generator, done);
    emit8(generator, 0x48); emit8(generator, 0x63); emit8(generator, 0xc0);
    emit8(generator, 0xc9);
    emit8(generator, 0xc3);
}

static int patch_fixups(Generator *generator)
{
    for (size_t i = 0; i < generator->fixup_count; i++) {
        Fixup fixup = generator->fixups[i];
        int64_t target;
        if (fixup.is_data) {
            target = (int64_t)generator->code.size +
                (int64_t)fixup.label;
        } else {
            if (fixup.label >= generator->label_count ||
                !generator->labels[fixup.label].defined) {
                fprintf(stderr, "S# Codegen: Undefined machine-code label.\n");
                generator->failed = 1;
                return 0;
            }
            target = (int64_t)generator->labels[fixup.label].position;
        }

        int64_t displacement = target -
            (int64_t)(fixup.displacement_offset + sizeof(int32_t));
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            fprintf(stderr, "S# Codegen: Relative displacement is out of range.\n");
            generator->failed = 1;
            return 0;
        }
        uint32_t encoded = (uint32_t)(int32_t)displacement;
        for (unsigned byte = 0; byte < 4; byte++)
            generator->code.bytes[fixup.displacement_offset + byte] =
                (unsigned char)(encoded >> (byte * 8));
    }
    return 1;
}

static void generator_cleanup(Generator *generator)
{
    pop_bindings(generator, NULL);
    free(generator->code.bytes);
    free(generator->data.bytes);
    free(generator->labels);
    free(generator->fixups);
    free(generator->function_labels);
    free(generator->function_epilogues);
}

int codegen_write_x86_64_elf(Stmt *program, const char *path)
{
    if (!program || program->type != STMT_BLOCK || !path)
        return 0;

    Generator generator = {0};
    generator.program = program;
    generator.statement_count = program->block.count;

    size_t helper_integer = new_label(&generator);
    size_t helper_string = new_label(&generator);
    size_t helper_string_compare = new_label(&generator);
    generator.function_labels = calloc(
        generator.statement_count + 3,
        sizeof(size_t)
    );
    generator.function_epilogues = calloc(
        generator.statement_count + 1,
        sizeof(size_t)
    );
    if (!generator.function_labels || !generator.function_epilogues) {
        fprintf(stderr, "S#: Out of memory allocating function labels.\n");
        generator.failed = 1;
    }

    for (size_t i = 0; i < generator.statement_count && !generator.failed; i++) {
        if (program->block.statements[i]->type == STMT_FUNCTION)
            generator.function_labels[i] = new_label(&generator);
    }
    if (!generator.failed) {
        generator.function_labels[generator.statement_count] = helper_integer;
        generator.function_labels[generator.statement_count + 1] = helper_string;
        generator.function_labels[generator.statement_count + 2] =
            helper_string_compare;
    }

    size_t entry_frame_slots = 0;
    for (size_t i = 0; i < generator.statement_count; i++) {
        Stmt *stmt = program->block.statements[i];
        if (stmt->type != STMT_FUNCTION)
            entry_frame_slots += count_statement_slots(stmt);
    }

    if (!generator.failed) {
        emit8(&generator, 0x48); emit8(&generator, 0x83);
        emit8(&generator, 0xe4); emit8(&generator, 0xf0);
        emit8(&generator, 0x55);
        emit8(&generator, 0x48); emit8(&generator, 0x89); emit8(&generator, 0xe5);
        size_t frame = aligned_frame(entry_frame_slots, 1);
        emit8(&generator, 0x48); emit8(&generator, 0x81); emit8(&generator, 0xec);
        emit32(&generator, (uint32_t)frame);

        for (size_t i = 0; i < generator.statement_count && !generator.failed; i++) {
            Stmt *stmt = program->block.statements[i];
            if (stmt->type != STMT_FUNCTION)
                emit_statement(&generator, stmt);
        }

        emit_mov_imm64(&generator, REG_RAX, 60);
        emit_mov_imm64(&generator, REG_RDI, 0);
        emit_syscall(&generator);
    }

    for (size_t i = 0; i < generator.statement_count && !generator.failed; i++) {
        Stmt *stmt = program->block.statements[i];
        if (stmt->type == STMT_FUNCTION)
            emit_function(&generator, stmt, i);
    }

    if (!generator.failed)
        emit_print_integer_helper(&generator, helper_integer);
    if (!generator.failed)
        emit_print_string_helper(&generator, helper_string);
    if (!generator.failed)
        emit_string_compare_helper(&generator, helper_string_compare);

    if (!generator.failed)
        patch_fixups(&generator);

    int result = 0;
    if (!generator.failed) {
        result = elf64_write_executable(
            path,
            generator.code.bytes,
            generator.code.size,
            generator.data.bytes,
            generator.data.size,
            0
        );
    }

    generator_cleanup(&generator);
    return result;
}
