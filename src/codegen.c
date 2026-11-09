#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "elf_writer.h"
#include "runtime/array_native.h"
#include "runtime/convert_native.h"
#include "runtime/string_native.h"

extern const unsigned char _binary_string_native_bin_start[];
extern const unsigned char _binary_string_native_bin_end[];
extern const unsigned char _binary_array_native_bin_start[];
extern const unsigned char _binary_array_native_bin_end[];
extern const unsigned char _binary_convert_native_bin_start[];
extern const unsigned char _binary_convert_native_bin_end[];

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
    REG_R10 = 10,
    REG_R11 = 11,
    REG_R12 = 12,
    REG_R13 = 13,
    REG_R14 = 14,
    REG_R15 = 15
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

static int is_convert_type_object(Expr *object)
{
    return object && object->type == EXPR_VARIABLE &&
        object->variable.name.length == 7 &&
        memcmp(object->variable.name.start, "Convert", 7) == 0;
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

static void emit_lea_rsp(Generator *generator, unsigned reg, size_t offset)
{
    emit8(generator, 0x48 | (reg >= 8 ? 0x04 : 0));
    emit8(generator, 0x8d);
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

static int token_is_text(Token token, const char *text);
static int is_string_type_object(Expr *object);
static int is_array_type_object(Expr *object);

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
                (expression_type(generator, expr->binary.left) == TYPE_STRING ||
                 expression_type(generator, expr->binary.right) == TYPE_STRING))
                return TYPE_STRING;
            return expression_type(generator, expr->binary.left);
        case EXPR_UNARY:
            return expr->unary.operator.type == TOK_BANG
                ? TYPE_BOOL
                : expression_type(generator, expr->unary.right);
        case EXPR_ASSIGN: {
            if (expr->assign.target)
                return expr->assign.target->type == EXPR_INDEX
                    ? array_element_type(expression_type(
                        generator,
                        expr->assign.target->index.object))
                    : expr->assign.target->type == EXPR_MEMBER
                    ? (expr->assign.target->member.name.length == 5 &&
                       memcmp(expr->assign.target->member.name.start,
                           "Title", 5) == 0
                        ? TYPE_STRING
                        : TYPE_INT)
                    : TYPE_ERROR;
            Binding *binding = find_binding(generator, expr->assign.name);
            return binding ? binding->type : TYPE_ERROR;
        }
        case EXPR_CALL: {
            if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                is_convert_type_object(
                    expr->call.callee->member.object)) {
                Token method = expr->call.callee->member.name;
                if (token_is_text(method, "ToByte") ||
                    token_is_text(method, "ToSByte") ||
                    token_is_text(method, "ToInt16") ||
                    token_is_text(method, "ToUInt16") ||
                    token_is_text(method, "ToInt32") ||
                    token_is_text(method, "ToUInt32") ||
                    token_is_text(method, "ToInt64"))
                    return TYPE_INT;
                if (token_is_text(method, "ToUInt64")) return TYPE_UINT64;
                if (token_is_text(method, "ToSingle")) return TYPE_FLOAT;
                if (token_is_text(method, "ToDouble") ||
                    token_is_text(method, "ToDecimal")) return TYPE_DOUBLE;
                if (token_is_text(method, "ToBoolean")) return TYPE_BOOL;
                if (token_is_text(method, "ToChar")) return TYPE_CHAR;
                if (token_is_text(method, "ToString")) return TYPE_STRING;
                return TYPE_ERROR;
            }
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
                ((expr->call.callee->member.name.length == 8 &&
                  memcmp(expr->call.callee->member.name.start,
                    "ReadLine", 8) == 0) ||
                 (expr->call.callee->member.name.length == 7 &&
                  memcmp(expr->call.callee->member.name.start,
                    "ReadKey", 7) == 0)))
                return TYPE_STRING;

            if (expr->call.callee &&
                expr->call.callee->type == EXPR_VARIABLE) {
                Stmt *function = find_function(
                    generator,
                    expr->call.callee->variable.name,
                    NULL
                );
                return function ? function->function.return_type : TYPE_ERROR;
            }
            if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                is_string_type_object(expr->call.callee->member.object)) {
                Token method = expr->call.callee->member.name;
                if (token_is_text(method, "IsNullOrEmpty") ||
                    token_is_text(method, "IsNullOrWhiteSpace"))
                    return TYPE_BOOL;
                if (token_is_text(method, "Format"))
                    return TYPE_STRING;
            }
            if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                is_array_type_object(expr->call.callee->member.object)) {
                Token method = expr->call.callee->member.name;
                if (token_is_text(method, "IndexOf") ||
                    token_is_text(method, "LastIndexOf"))
                    return TYPE_INT;
                if (token_is_text(method, "Resize") &&
                    expr->call.argument_count)
                    return expression_type(generator,
                        expr->call.arguments[0]);
                return TYPE_VOID;
            }
            if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                expr->call.callee->member.object &&
                expression_type(generator,
                    expr->call.callee->member.object) == TYPE_STRING) {
                Token method = expr->call.callee->member.name;
                if ((method.length == 8 &&
                     memcmp(method.start, "Contains", 8) == 0) ||
                    (method.length == 10 &&
                     memcmp(method.start, "StartsWith", 10) == 0) ||
                    (method.length == 8 &&
                     memcmp(method.start, "EndsWith", 8) == 0))
                    return TYPE_BOOL;
                if ((method.length == 7 &&
                     memcmp(method.start, "IndexOf", 7) == 0) ||
                    (method.length == 11 &&
                     memcmp(method.start, "LastIndexOf", 11) == 0))
                    return TYPE_INT;
                return TYPE_STRING;
            }
            return TYPE_VOID;
        }
        case EXPR_MEMBER:
            if (expr->member.object &&
                expr->member.object->type == EXPR_VARIABLE &&
                expr->member.object->variable.name.length == 7 &&
                memcmp(expr->member.object->variable.name.start,
                    "Console", 7) == 0) {
                if (expr->member.name.length == 5 &&
                    memcmp(expr->member.name.start, "Title", 5) == 0)
                    return TYPE_STRING;
                return TYPE_INT;
            }
            if (expression_type(generator, expr->member.object) == TYPE_STRING &&
                expr->member.name.length == 6 &&
                memcmp(expr->member.name.start, "Length", 6) == 0)
                return TYPE_INT;
            if (is_array_type(expression_type(generator, expr->member.object)) &&
                expr->member.name.length == 6 &&
                memcmp(expr->member.name.start, "Length", 6) == 0)
                return TYPE_INT;
            return TYPE_ERROR;
        case EXPR_INDEX:
            return array_element_type(
                expression_type(generator, expr->index.object));
        case EXPR_ARRAY_LITERAL:
            return TYPE_ERROR;
        case EXPR_ARRAY_CREATE:
            return expr->array_create.data_type;
    }
    return TYPE_ERROR;
}

static int supported_native_type(
    Generator *generator,
    DataType type,
    Token location)
{
    if (type == TYPE_INT || type == TYPE_UINT64 || type == TYPE_CHAR ||
        type == TYPE_FLOAT || type == TYPE_DOUBLE || type == TYPE_BOOL ||
        type == TYPE_STRING || type == TYPE_NULL ||
        is_array_type(type) ||
        type == TYPE_VOID || type == TYPE_ERROR)
        return 1;

    report_error(
        generator,
        location,
        "Direct ELF backend does not support this value type"
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

static size_t append_string_segment(
    Generator *generator,
    Token token,
    size_t start,
    size_t end)
{
    size_t offset = generator->data.size;
    for (size_t i = start; i < end && !generator->failed; i++) {
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
    return offset;
}

static void emit_expression(Generator *generator, Expr *expr);
static void emit_statement(Generator *generator, Stmt *stmt);

static int is_console_call(Expr *call, const char *name)
{
    Expr *callee = call && call->type == EXPR_CALL
        ? call->call.callee
        : NULL;
    if (!callee || callee->type != EXPR_MEMBER ||
        !callee->member.object ||
        callee->member.object->type != EXPR_VARIABLE)
        return 0;

    Token object = callee->member.object->variable.name;
    Token member = callee->member.name;
    size_t name_length = strlen(name);
    return object.length == 7 &&
        memcmp(object.start, "Console", 7) == 0 &&
        member.length == name_length &&
        memcmp(member.start, name, name_length) == 0;
}

static void emit_call_aligned(Generator *generator, size_t label)
{
    size_t padding = generator->stack_depth % 16 == 0 ? 0 : 8;
    if (padding)
        emit_stack_adjust(generator, 1, padding);
    emit_call_label(generator, label);
    if (padding)
        emit_stack_adjust(generator, 0, padding);
}

static int token_is_text(Token token, const char *text)
{
    size_t length = strlen(text);
    return token.length == length &&
        memcmp(token.start, text, length) == 0;
}

static long long convert_operation(Token name)
{
    if (token_is_text(name, "ToByte")) return SSHARP_CONVERT_TO_BYTE;
    if (token_is_text(name, "ToSByte")) return SSHARP_CONVERT_TO_SBYTE;
    if (token_is_text(name, "ToInt16")) return SSHARP_CONVERT_TO_INT16;
    if (token_is_text(name, "ToUInt16")) return SSHARP_CONVERT_TO_UINT16;
    if (token_is_text(name, "ToInt32")) return SSHARP_CONVERT_TO_INT32;
    if (token_is_text(name, "ToUInt32")) return SSHARP_CONVERT_TO_UINT32;
    if (token_is_text(name, "ToInt64")) return SSHARP_CONVERT_TO_INT64;
    if (token_is_text(name, "ToUInt64")) return SSHARP_CONVERT_TO_UINT64;
    if (token_is_text(name, "ToSingle")) return SSHARP_CONVERT_TO_SINGLE;
    if (token_is_text(name, "ToDouble")) return SSHARP_CONVERT_TO_DOUBLE;
    if (token_is_text(name, "ToDecimal")) return SSHARP_CONVERT_TO_DECIMAL;
    if (token_is_text(name, "ToBoolean")) return SSHARP_CONVERT_TO_BOOLEAN;
    if (token_is_text(name, "ToChar")) return SSHARP_CONVERT_TO_CHAR;
    if (token_is_text(name, "ToString")) return SSHARP_CONVERT_TO_STRING;
    return 0;
}

static uint64_t convert_type_tag(DataType type)
{
    if (type == TYPE_INT) return SSHARP_CONVERT_INT;
    if (type == TYPE_BOOL) return SSHARP_CONVERT_BOOL;
    if (type == TYPE_STRING) return SSHARP_CONVERT_STRING;
    if (type == TYPE_DOUBLE) return SSHARP_CONVERT_DOUBLE;
    if (type == TYPE_FLOAT) return SSHARP_CONVERT_FLOAT;
    if (type == TYPE_CHAR) return SSHARP_CONVERT_CHAR;
    if (type == TYPE_UINT64) return SSHARP_CONVERT_UINT64;
    if (type == TYPE_NULL) return SSHARP_CONVERT_NULL;
    return 0;
}

static void emit_convert_call(Generator *generator, Expr *call)
{
    Token method = call->call.callee->member.name;
    long long operation = convert_operation(method);
    if (!operation || call->call.argument_count != 1) {
        report_error(generator, method, "Invalid Convert method call");
        return;
    }

    Expr *argument = call->call.arguments[0];
    uint64_t type_tag = convert_type_tag(expression_type(generator, argument));
    if (!type_tag) {
        report_error(generator, method, "Unsupported Convert argument type");
        return;
    }
    emit_expression(generator, argument);
    emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
    emit_mov_imm64(generator, REG_RSI, type_tag);
    emit_mov_imm64(generator, REG_R9, (uint64_t)operation);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 9]);
}

static void emit_convert_to_string(Generator *generator, Expr *value)
{
    uint64_t type_tag = convert_type_tag(expression_type(generator, value));
    if (!type_tag) {
        report_error(generator, (Token){0},
            "Unsupported value type for Console output");
        return;
    }
    emit_expression(generator, value);
    emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
    emit_mov_imm64(generator, REG_RSI, type_tag);
    emit_mov_imm64(generator, REG_R9, SSHARP_CONVERT_TO_STRING);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 9]);
}

static int is_string_type_object(Expr *object)
{
    return object && object->type == EXPR_VARIABLE &&
        token_is_text(object->variable.name, "String");
}

static int is_array_type_object(Expr *object)
{
    return object && object->type == EXPR_VARIABLE &&
        token_is_text(object->variable.name, "Array");
}

static long long string_operation(Token name)
{
    if (token_is_text(name, "Contains")) return SSHARP_STRING_CONTAINS;
    if (token_is_text(name, "StartsWith")) return SSHARP_STRING_STARTS_WITH;
    if (token_is_text(name, "EndsWith")) return SSHARP_STRING_ENDS_WITH;
    if (token_is_text(name, "IndexOf")) return SSHARP_STRING_INDEX_OF;
    if (token_is_text(name, "LastIndexOf")) return SSHARP_STRING_LAST_INDEX_OF;
    if (token_is_text(name, "Replace")) return SSHARP_STRING_REPLACE;
    if (token_is_text(name, "Substring")) return SSHARP_STRING_SUBSTRING;
    if (token_is_text(name, "Trim")) return SSHARP_STRING_TRIM;
    if (token_is_text(name, "TrimStart")) return SSHARP_STRING_TRIM_START;
    if (token_is_text(name, "TrimEnd")) return SSHARP_STRING_TRIM_END;
    if (token_is_text(name, "ToUpper")) return SSHARP_STRING_TO_UPPER;
    if (token_is_text(name, "ToLower")) return SSHARP_STRING_TO_LOWER;
    if (token_is_text(name, "IsNullOrEmpty"))
        return SSHARP_STRING_IS_NULL_OR_EMPTY;
    if (token_is_text(name, "IsNullOrWhiteSpace"))
        return SSHARP_STRING_IS_NULL_OR_WHITE_SPACE;
    return 0;
}

static void emit_string_native_call(
    Generator *generator,
    Expr *receiver,
    Expr **arguments,
    size_t argument_count,
    long long operation)
{
    emit_expression(generator, receiver);
    emit_push(generator, REG_RAX);
    for (size_t i = 0; i < argument_count; i++) {
        emit_expression(generator, arguments[i]);
        emit_push(generator, REG_RAX);
    }

    size_t padding = generator->stack_depth % 16 == 0 ? 0 : 8;
    if (padding)
        emit_stack_adjust(generator, 1, padding);

    emit_load_rsp(generator, REG_RDI, padding + argument_count * 8);
    if (argument_count > 0) {
        size_t first_offset = padding + (argument_count - 1) * 8;
        if (operation == SSHARP_STRING_SUBSTRING)
            emit_load_rsp(generator, REG_RCX, first_offset);
        else
            emit_load_rsp(generator, REG_RSI, first_offset);
    }
    if (argument_count > 1) {
        size_t second_offset = padding;
        if (operation == SSHARP_STRING_SUBSTRING)
            emit_load_rsp(generator, REG_R8, second_offset);
        else
            emit_load_rsp(generator, REG_RDX, second_offset);
    }
    if (operation == SSHARP_STRING_SUBSTRING && argument_count == 1)
        emit_mov_imm64(generator, REG_R8, (uint64_t)-1);
    emit_mov_imm64(generator, REG_R9, (uint64_t)operation);
    emit_call_label(generator, generator->function_labels[
        generator->statement_count + 7]);

    if (padding)
        emit_stack_adjust(generator, 0, padding);
    emit_stack_adjust(generator, 0, (argument_count + 1) * 8);
}

static void emit_string_format_call(Generator *generator, Expr *call)
{
    size_t argument_count = call->call.argument_count - 1;
    if (argument_count > 64) {
        report_error(generator, call->call.callee->member.name,
            "String.Format supports at most 64 values");
        return;
    }

    emit_expression(generator, call->call.arguments[0]);
    emit_push(generator, REG_RAX);
    for (size_t i = 0; i < argument_count; i++) {
        Expr *argument = call->call.arguments[i + 1];
        DataType type = expression_type(generator, argument);
        uint64_t type_tag = type == TYPE_STRING || type == TYPE_NULL ? 1 :
            (type == TYPE_INT ? 2 : (type == TYPE_BOOL ? 3 : 0));
        if (!type_tag) {
            report_error(generator, call->call.callee->member.name,
                "String.Format supports string, integer, and boolean values");
            return;
        }
        emit_expression(generator, argument);
        emit_push(generator, REG_RAX);
        emit_mov_imm64(generator, REG_RAX, type_tag);
        emit_push(generator, REG_RAX);
    }

    size_t padding = generator->stack_depth % 16 == 0 ? 0 : 8;
    if (padding)
        emit_stack_adjust(generator, 1, padding);
    emit_load_rsp(generator, REG_RDI, padding + argument_count * 16);
    if (argument_count)
        emit_lea_rsp(generator, REG_RSI,
            padding + (argument_count - 1) * 16);
    else
        emit_mov_imm64(generator, REG_RSI, 0);
    emit_mov_imm64(generator, REG_RCX, argument_count);
    emit_mov_imm64(generator, REG_R9, SSHARP_STRING_FORMAT);
    emit_call_label(generator, generator->function_labels[
        generator->statement_count + 7]);

    if (padding)
        emit_stack_adjust(generator, 0, padding);
    emit_stack_adjust(generator, 0, (argument_count * 2 + 1) * 8);
}

static void emit_array_allocation(
    Generator *generator,
    size_t length)
{
    emit_mov_imm64(generator, REG_RCX, length);
    emit_mov_imm64(generator, REG_R9, SSHARP_ARRAY_ALLOCATE);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 8]);
}

static void emit_array_literal(
    Generator *generator,
    Expr *literal)
{
    emit_array_allocation(generator, literal->array_literal.count);
    emit_push(generator, REG_RAX);

    for (size_t i = 0;
         i < literal->array_literal.count && !generator->failed;
         i++) {
        emit_expression(generator, literal->array_literal.elements[i]);
        emit_mov_reg_reg(generator, REG_RDX, REG_RAX);
        emit_load_rsp(generator, REG_RDI, 0);
        emit_mov_imm64(generator, REG_RSI, i);
        emit_mov_imm64(generator, REG_R9, SSHARP_ARRAY_SET);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count + 8]);
    }

    emit_pop(generator, REG_RAX);
}

static void emit_array_index(Generator *generator, Expr *expr)
{
    emit_expression(generator, expr->index.object);
    emit_push(generator, REG_RAX);
    emit_expression(generator, expr->index.index);
    emit_mov_reg_reg(generator, REG_RSI, REG_RAX);
    emit_pop(generator, REG_RDI);
    emit_mov_imm64(generator, REG_R9, SSHARP_ARRAY_GET);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 8]);
}

static void emit_array_assignment(Generator *generator, Expr *expr)
{
    Expr *target = expr->assign.target;
    emit_expression(generator, target->index.object);
    emit_push(generator, REG_RAX);
    emit_expression(generator, target->index.index);
    emit_push(generator, REG_RAX);
    emit_expression(generator, expr->assign.value);
    emit_mov_reg_reg(generator, REG_RDX, REG_RAX);
    emit_load_rsp(generator, REG_RSI, 0);
    emit_load_rsp(generator, REG_RDI, 8);
    emit_mov_imm64(generator, REG_R9, SSHARP_ARRAY_SET);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 8]);
    emit_stack_adjust(generator, 0, 16);
}

static void emit_array_method_call(Generator *generator, Expr *call)
{
    Token method = call->call.callee->member.name;
    size_t count = call->call.argument_count;
    DataType array_type = expression_type(generator, call->call.arguments[0]);
    long long operation = 0;

    if (token_is_text(method, "Reverse"))
        operation = SSHARP_ARRAY_REVERSE;
    else if (token_is_text(method, "Sort"))
        operation = array_type == TYPE_INT_ARRAY
            ? SSHARP_ARRAY_SORT_INT
            : (array_type == TYPE_BOOL_ARRAY
                ? SSHARP_ARRAY_SORT_BOOL
                : SSHARP_ARRAY_SORT_STRING);
    else if (token_is_text(method, "IndexOf"))
        operation = array_type == TYPE_INT_ARRAY
            ? SSHARP_ARRAY_INDEX_OF_INT
            : (array_type == TYPE_BOOL_ARRAY
                ? SSHARP_ARRAY_INDEX_OF_BOOL
                : SSHARP_ARRAY_INDEX_OF_STRING);
    else if (token_is_text(method, "LastIndexOf"))
        operation = array_type == TYPE_INT_ARRAY
            ? SSHARP_ARRAY_LAST_INDEX_OF_INT
            : (array_type == TYPE_BOOL_ARRAY
                ? SSHARP_ARRAY_LAST_INDEX_OF_BOOL
                : SSHARP_ARRAY_LAST_INDEX_OF_STRING);
    else if (token_is_text(method, "Copy"))
        operation = SSHARP_ARRAY_COPY;
    else if (token_is_text(method, "Resize"))
        operation = SSHARP_ARRAY_RESIZE;
    else if (token_is_text(method, "Clear"))
        operation = SSHARP_ARRAY_CLEAR;
    else if (token_is_text(method, "Fill"))
        operation = count == 2
            ? SSHARP_ARRAY_FILL_ALL
            : SSHARP_ARRAY_FILL;

    if (!operation) {
        report_error(generator, method, "Unsupported Array method");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        emit_expression(generator, call->call.arguments[i]);
        emit_push(generator, REG_RAX);
    }
    size_t padding = generator->stack_depth % 16 == 0 ? 0 : 8;
    if (padding)
        emit_stack_adjust(generator, 1, padding);

    if (token_is_text(method, "Resize")) {
        emit_load_rsp(generator, REG_RDI, padding + 8);
        emit_load_rsp(generator, REG_RCX, padding);
    } else if (token_is_text(method, "Clear")) {
        emit_load_rsp(generator, REG_RDI, padding + 16);
        emit_load_rsp(generator, REG_RSI, padding + 8);
        emit_load_rsp(generator, REG_RCX, padding);
    } else if (token_is_text(method, "Fill")) {
        emit_load_rsp(generator, REG_RDI, padding + (count - 1) * 8);
        if (count == 2) {
            emit_mov_imm64(generator, REG_RSI, 0);
            emit_load_rsp(generator, REG_RDX, padding);
            emit_mov_imm64(generator, REG_RCX, 0);
        } else {
            emit_load_rsp(generator, REG_RSI, padding + 8);
            emit_load_rsp(generator, REG_RDX, padding + 16);
            emit_load_rsp(generator, REG_RCX, padding);
        }
    } else if (token_is_text(method, "IndexOf") ||
               token_is_text(method, "LastIndexOf")) {
        emit_load_rsp(generator, REG_RDI, padding + 8);
        emit_load_rsp(generator, REG_RDX, padding);
    } else {
        static const unsigned registers[] = {
            REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8
        };
        for (size_t i = 0; i < count && i < 5; i++)
            emit_load_rsp(generator, registers[i],
                padding + (count - 1 - i) * 8);
    }

    emit_mov_imm64(generator, REG_R9, (uint64_t)operation);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 8]);

    if (padding)
        emit_stack_adjust(generator, 0, padding);
    if (count)
        emit_stack_adjust(generator, 0, count * 8);
}

static void emit_print_value(
    Generator *generator,
    Expr *argument,
    int append_newline)
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
    if (is_array_type(type)) {
        report_error(generator, (Token){0},
            "Array values cannot be written directly");
        return;
    }

    if (argument) {
        emit_expression(generator, argument);
    } else {
        emit_mov_imm64(generator, REG_RAX, 0);
    }
    emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
    emit_mov_imm64(generator, REG_RSI, append_newline != 0);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 1]);
}

static void emit_write_value(
    Generator *generator,
    Expr *argument,
    int append_newline)
{
    DataType type = argument
        ? expression_type(generator, argument)
        : TYPE_STRING;
    Token location = argument && argument->type == EXPR_LITERAL
        ? argument->literal.value
        : argument && argument->type == EXPR_VARIABLE
            ? argument->variable.name
            : (Token){0};

    if (!supported_native_type(generator, type, location))
        return;
    if (type == TYPE_VOID || type == TYPE_ERROR) {
        report_error(generator, location,
            "Console output argument has no printable value");
        return;
    }
    if (is_array_type(type)) {
        report_error(generator, location,
            "Array values cannot be written directly");
        return;
    }

    if (type == TYPE_BOOL) {
        size_t false_label = new_label(generator);
        size_t done_label = new_label(generator);
        emit_expression(generator, argument);
        emit_test_rax(generator);
        emit_conditional_jump(generator, 0x84, false_label);
        size_t true_data = append_static_string(generator, "true");
        emit_lea_rdi_data(generator, true_data);
        emit_mov_imm64(generator, REG_RSI, append_newline != 0);
        emit_jump(generator, done_label);
        define_label(generator, false_label);
        size_t false_data = append_static_string(generator, "false");
        emit_lea_rdi_data(generator, false_data);
        emit_mov_imm64(generator, REG_RSI, append_newline != 0);
        define_label(generator, done_label);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count + 1]);
        return;
    }

    if (type == TYPE_INT) {
        emit_expression(generator, argument);
        emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
        emit_mov_imm64(generator, REG_RSI, append_newline != 0);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count]);
        return;
    }

    if (type == TYPE_UINT64 || type == TYPE_CHAR ||
        type == TYPE_FLOAT || type == TYPE_DOUBLE) {
        emit_convert_to_string(generator, argument);
        emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
        emit_mov_imm64(generator, REG_RSI, append_newline != 0);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count + 1]);
        return;
    }

    emit_print_value(generator, argument, append_newline);
}

static void emit_console_static_text(Generator *generator, const char *text);

static int has_composite_format_marker(Token token)
{
    size_t end = token.length ? token.length - 1 : 0;
    for (size_t i = 1; i < end; i++) {
        if (token.start[i] == '{' || token.start[i] == '}')
            return 1;
    }
    return 0;
}

static void emit_console_output(Generator *generator, Expr *call, int newline)
{
    if (call->call.argument_count == 0) {
        if (newline) {
            size_t empty_string = append_static_string(generator, "");
            emit_lea_rdi_data(generator, empty_string);
            emit_mov_imm64(generator, REG_RSI, 1);
            emit_call_aligned(generator, generator->function_labels[
                generator->statement_count + 1]);
        }
        return;
    }

    Expr *format = call->call.arguments[0];
    if (call->call.argument_count > 1 &&
        format->type == EXPR_LITERAL &&
        format->literal.value.type == TOK_STRING &&
        has_composite_format_marker(format->literal.value)) {
        Token text = format->literal.value;
        size_t end = text.length ? text.length - 1 : 0;
        size_t cursor = 1;
        size_t segment_start = cursor;

        while (cursor < end && !generator->failed) {
            if (text.start[cursor] == '{' &&
                cursor + 1 < end && text.start[cursor + 1] == '{') {
                if (cursor > segment_start) {
                    size_t data = append_string_segment(
                        generator, text, segment_start, cursor);
                    emit_lea_rdi_data(generator, data);
                    emit_mov_imm64(generator, REG_RSI, 0);
                    emit_call_aligned(generator, generator->function_labels[
                        generator->statement_count + 1]);
                }
                emit_console_static_text(generator, "{");
                cursor += 2;
                segment_start = cursor;
                continue;
            }

            if (text.start[cursor] == '}' &&
                cursor + 1 < end && text.start[cursor + 1] == '}') {
                if (cursor > segment_start) {
                    size_t data = append_string_segment(
                        generator, text, segment_start, cursor);
                    emit_lea_rdi_data(generator, data);
                    emit_mov_imm64(generator, REG_RSI, 0);
                    emit_call_aligned(generator, generator->function_labels[
                        generator->statement_count + 1]);
                }
                emit_console_static_text(generator, "}");
                cursor += 2;
                segment_start = cursor;
                continue;
            }

            if (text.start[cursor] == '}') {
                report_error(generator, text, "Unescaped closing brace in format string");
                return;
            }

            if (text.start[cursor] != '{') {
                cursor++;
                continue;
            }

            if (cursor > segment_start) {
                size_t data = append_string_segment(
                    generator, text, segment_start, cursor);
                emit_lea_rdi_data(generator, data);
                emit_mov_imm64(generator, REG_RSI, 0);
                emit_call_aligned(generator, generator->function_labels[
                    generator->statement_count + 1]);
            }

            size_t placeholder = cursor + 1;
            if (placeholder >= end ||
                text.start[placeholder] < '0' ||
                text.start[placeholder] > '9') {
                report_error(generator, text, "Invalid composite format item");
                return;
            }

            size_t argument_index = 0;
            while (placeholder < end &&
                text.start[placeholder] >= '0' &&
                text.start[placeholder] <= '9') {
                size_t digit = (size_t)(text.start[placeholder] - '0');
                if (argument_index > (SIZE_MAX - digit) / 10) {
                    report_error(generator, text, "Format item index is too large");
                    return;
                }
                argument_index = argument_index * 10 + digit;
                placeholder++;
            }

            if (placeholder >= end || text.start[placeholder] != '}' ||
                argument_index >= call->call.argument_count - 1) {
                report_error(generator, text,
                    "Composite format item has no matching argument");
                return;
            }

            emit_write_value(
                generator,
                call->call.arguments[argument_index + 1],
                0
            );
            cursor = placeholder + 1;
            segment_start = cursor;
        }

        if (segment_start < end) {
            size_t data = append_string_segment(
                generator, text, segment_start, end);
            emit_lea_rdi_data(generator, data);
            emit_mov_imm64(generator, REG_RSI, 0);
            emit_call_aligned(generator, generator->function_labels[
                generator->statement_count + 1]);
        }

        if (newline) {
            size_t empty_string = append_static_string(generator, "");
            emit_lea_rdi_data(generator, empty_string);
            emit_mov_imm64(generator, REG_RSI, 1);
            emit_call_aligned(generator, generator->function_labels[
                generator->statement_count + 1]);
        }
        return;
    }

    if (call->call.argument_count > 1 &&
        format->type != EXPR_LITERAL &&
        expression_type(generator, format) == TYPE_STRING) {
        report_error(generator, (Token){0},
            "Composite formatting requires a string literal");
        return;
    }

    for (size_t i = 0; i < call->call.argument_count; i++) {
        emit_write_value(
            generator,
            call->call.arguments[i],
            newline && i + 1 == call->call.argument_count
        );
    }
}

static void emit_console_static_text(
    Generator *generator,
    const char *text)
{
    size_t data = append_static_string(generator, text);
    emit_lea_rdi_data(generator, data);
    emit_mov_imm64(generator, REG_RSI, 0);
    emit_call_aligned(generator, generator->function_labels[
        generator->statement_count + 1]);
}

static void emit_console_property_assignment(
    Generator *generator,
    Expr *assignment)
{
    Expr *target = assignment->assign.target;
    Token property = target->member.name;

    if (property.length == 5 &&
        memcmp(property.start, "Title", 5) == 0) {
        emit_console_static_text(generator, "\033]0;");
        emit_expression(generator, assignment->assign.value);
        emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
        emit_mov_imm64(generator, REG_RSI, 0);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count + 1]);
        emit_console_static_text(generator, "\007");
        return;
    }

    int background = property.length == 15 &&
        memcmp(property.start, "BackgroundColor", 15) == 0;
    int foreground = property.length == 15 &&
        memcmp(property.start, "ForegroundColor", 15) == 0;
    if (background || foreground) {
        emit_expression(generator, assignment->assign.value);
        emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
        emit_mov_imm64(generator, REG_RSI, background);
        emit_call_aligned(generator, generator->function_labels[
            generator->statement_count + 5]);
        return;
    }

    report_error(generator, property, "Unsupported Console property setter");
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
        emit_expression(generator, expr->binary.left);
        emit_push(generator, REG_RAX);
        emit_expression(generator, expr->binary.right);
        emit_push(generator, REG_RAX);
        size_t padding = generator->stack_depth % 16 == 0 ? 0 : 8;
        if (padding)
            emit_stack_adjust(generator, 1, padding);
        emit_load_rsp(generator, REG_RDI, padding + 8);
        emit_load_rsp(generator, REG_RSI, padding);
        emit_mov_imm64(generator, REG_R9, SSHARP_STRING_CONCAT);
        emit_call_label(generator, generator->function_labels[
            generator->statement_count + 7]);
        if (padding)
            emit_stack_adjust(generator, 0, padding);
        emit_stack_adjust(generator, 0, 16);
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
            if (expr->assign.target) {
                if (expr->assign.target->type == EXPR_INDEX)
                    emit_array_assignment(generator, expr);
                else
                    emit_console_property_assignment(generator, expr);
                break;
            }

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
            if (is_console_call(expr, "Write"))
                emit_console_output(generator, expr, 0);
            else if (is_console_call(expr, "WriteLine"))
                emit_console_output(generator, expr, 1);
            else if (is_console_call(expr, "ReadLine")) {
                if (expr->call.argument_count != 0) {
                    report_error(generator,
                        expr->call.callee->member.name,
                        "Console.ReadLine accepts no arguments");
                    return;
                }
                emit_call_aligned(generator, generator->function_labels[
                    generator->statement_count + 3]);
            }
            else if (is_console_call(expr, "ReadKey")) {
                if (expr->call.argument_count != 0) {
                    report_error(generator,
                        expr->call.callee->member.name,
                        "Console.ReadKey accepts no arguments");
                    return;
                }
                emit_call_aligned(generator, generator->function_labels[
                    generator->statement_count + 4]);
            }
            else if (is_console_call(expr, "SetCursorPosition")) {
                if (expr->call.argument_count != 2) {
                    report_error(generator,
                        expr->call.callee->member.name,
                        "Console.SetCursorPosition requires two arguments");
                    return;
                }
                emit_expression(generator, expr->call.arguments[0]);
                emit_push(generator, REG_RAX);
                emit_expression(generator, expr->call.arguments[1]);
                emit_mov_reg_reg(generator, REG_RSI, REG_RAX);
                emit_pop(generator, REG_RDI);
                emit_call_aligned(generator, generator->function_labels[
                    generator->statement_count + 6]);
            }
            else if (is_console_call(expr, "Clear")) {
                if (expr->call.argument_count != 0) {
                    report_error(generator,
                        expr->call.callee->member.name,
                        "Console.Clear accepts no arguments");
                    return;
                }
                emit_console_static_text(generator, "\033[2J\033[H");
            }
            else if (is_console_call(expr, "ResetColor")) {
                if (expr->call.argument_count != 0) {
                    report_error(generator,
                        expr->call.callee->member.name,
                        "Console.ResetColor accepts no arguments");
                    return;
                }
                emit_console_static_text(generator, "\033[0m");
            }
            else if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                is_convert_type_object(expr->call.callee->member.object))
                emit_convert_call(generator, expr);
            else if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                is_array_type_object(expr->call.callee->member.object))
                emit_array_method_call(generator, expr);
            else if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                is_string_type_object(expr->call.callee->member.object)) {
                Token method = expr->call.callee->member.name;
                if (token_is_text(method, "Format")) {
                    if (!expr->call.argument_count) {
                        report_error(generator, method,
                            "String.Format requires a format string");
                        return;
                    }
                    emit_string_format_call(generator, expr);
                } else {
                    long long operation = string_operation(method);
                    if (!operation || expr->call.argument_count != 1) {
                        report_error(generator, method,
                            "Invalid static String method call");
                        return;
                    }
                    emit_expression(generator, expr->call.arguments[0]);
                    emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
                    emit_mov_imm64(generator, REG_R9, (uint64_t)operation);
                    emit_call_aligned(generator, generator->function_labels[
                        generator->statement_count + 7]);
                }
            }
            else if (expr->call.callee &&
                expr->call.callee->type == EXPR_MEMBER &&
                expression_type(generator,
                    expr->call.callee->member.object) == TYPE_STRING) {
                long long operation =
                    string_operation(expr->call.callee->member.name);
                if (!operation) {
                    report_error(generator,
                        expr->call.callee->member.name,
                        "Unsupported String method");
                    return;
                }
                emit_string_native_call(
                    generator,
                    expr->call.callee->member.object,
                    expr->call.arguments,
                    expr->call.argument_count,
                    operation);
            }
            else
                emit_function_call(generator, expr);
            break;

        case EXPR_MEMBER:
            if (expression_type(generator, expr->member.object) == TYPE_STRING &&
                token_is_text(expr->member.name, "Length")) {
                emit_string_native_call(
                    generator, expr->member.object, NULL, 0,
                    SSHARP_STRING_LENGTH);
            } else if (is_array_type(
                    expression_type(generator, expr->member.object)) &&
                token_is_text(expr->member.name, "Length")) {
                emit_expression(generator, expr->member.object);
                emit_mov_reg_reg(generator, REG_RDI, REG_RAX);
                emit_mov_imm64(generator, REG_R9, SSHARP_ARRAY_LENGTH);
                emit_call_aligned(generator, generator->function_labels[
                    generator->statement_count + 8]);
            } else {
                report_error(generator, expr->member.name,
                    "Unsupported member expression");
            }
            break;

        case EXPR_INDEX:
            emit_array_index(generator, expr);
            break;

        case EXPR_ARRAY_LITERAL:
            report_error(generator, (Token){0},
                "Array initializer is only supported in an array declaration");
            break;

        case EXPR_ARRAY_CREATE:
            emit_expression(generator, expr->array_create.length);
            emit_mov_reg_reg(generator, REG_RCX, REG_RAX);
            emit_mov_imm64(generator, REG_R9, SSHARP_ARRAY_ALLOCATE);
            emit_call_aligned(generator, generator->function_labels[
                generator->statement_count + 8]);
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
            if (stmt->variable.initializer &&
                stmt->variable.initializer->type == EXPR_ARRAY_LITERAL)
                emit_array_literal(generator, stmt->variable.initializer);
            else if (stmt->variable.initializer)
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

    emit8(generator, 0x48); emit8(generator, 0x85); emit8(generator, 0xf6);
    size_t no_newline = new_label(generator);
    size_t start_digits = new_label(generator);
    emit_conditional_jump(generator, 0x84, no_newline);

    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xee);
    emit8(generator, 0x48); emit8(generator, 0x83); emit8(generator, 0xee);
    emit8(generator, 1);
    emit8(generator, 0xc6); emit8(generator, 0x06); emit8(generator, '\n');
    emit8(generator, 0x49); emit8(generator, 0xc7); emit8(generator, 0xc1);
    emit32(generator, 1);
    emit_jump(generator, start_digits);

    define_label(generator, no_newline);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xee);
    emit8(generator, 0x48); emit8(generator, 0x83); emit8(generator, 0xee);
    emit8(generator, 1);
    emit8(generator, 0x4d); emit8(generator, 0x31); emit8(generator, 0xc9);
    define_label(generator, start_digits);

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
    emit_store_rbp(generator, 16, REG_RSI);
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

    emit_load_rbp(generator, REG_RAX, 16);
    emit_test_rax(generator);
    size_t no_newline = new_label(generator);
    emit_conditional_jump(generator, 0x84, no_newline);
    emit8(generator, 0xc6); emit8(generator, 0x45); emit8(generator, 0xff);
    emit8(generator, '\n');
    emit8(generator, 0x48); emit8(generator, 0x8d); emit8(generator, 0x75);
    emit8(generator, 0xff);
    emit_mov_imm64(generator, REG_RDX, 1);
    emit_write_syscall(generator, 1);
    define_label(generator, no_newline);
    emit8(generator, 0xc9);
    emit8(generator, 0xc3);
}

static void emit_read_line_helper(Generator *generator, size_t label)
{
    define_label(generator, label);
    emit8(generator, 0x55);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xe5);
    emit8(generator, 0x41); emit8(generator, 0x54);
    emit8(generator, 0x41); emit8(generator, 0x55);
    emit8(generator, 0x41); emit8(generator, 0x56);

    emit8(generator, 0xb8); emit32(generator, 9);
    emit8(generator, 0x31); emit8(generator, 0xff);
    emit8(generator, 0xbe); emit32(generator, 4096);
    emit8(generator, 0xba); emit32(generator, 3);
    emit8(generator, 0x41); emit8(generator, 0xba); emit32(generator, 0x22);
    emit8(generator, 0x49); emit8(generator, 0xc7);
    emit8(generator, 0xc0); emit32(generator, UINT32_MAX);
    emit8(generator, 0x45); emit8(generator, 0x31); emit8(generator, 0xc9);
    emit_syscall(generator);
    emit_test_rax(generator);
    size_t allocation_failed = new_label(generator);
    size_t read_loop = new_label(generator);
    size_t read_ready = new_label(generator);
    size_t line_done = new_label(generator);
    size_t terminate_line = new_label(generator);
    size_t input_done = new_label(generator);
    size_t helper_done = new_label(generator);
    emit_conditional_jump(generator, 0x88, allocation_failed);

    emit8(generator, 0x49); emit8(generator, 0x89); emit8(generator, 0xc4);
    emit8(generator, 0x45); emit8(generator, 0x31); emit8(generator, 0xed);
    emit8(generator, 0x41); emit8(generator, 0xbe); emit32(generator, 4096);
    define_label(generator, read_loop);
    emit8(generator, 0x49); emit8(generator, 0x8d);
    emit8(generator, 0x45); emit8(generator, 1);
    emit8(generator, 0x4c); emit8(generator, 0x39); emit8(generator, 0xf0);
    emit_conditional_jump(generator, 0x82, read_ready);

    emit8(generator, 0xb8); emit32(generator, 25);
    emit8(generator, 0x4c); emit8(generator, 0x89); emit8(generator, 0xe7);
    emit8(generator, 0x4c); emit8(generator, 0x89); emit8(generator, 0xf6);
    emit8(generator, 0x4c); emit8(generator, 0x89); emit8(generator, 0xf2);
    emit8(generator, 0x48); emit8(generator, 0xd1); emit8(generator, 0xe2);
    emit8(generator, 0x41); emit8(generator, 0xba); emit32(generator, 1);
    emit_syscall(generator);
    emit_test_rax(generator);
    emit_conditional_jump(generator, 0x88, allocation_failed);
    emit8(generator, 0x49); emit8(generator, 0x89); emit8(generator, 0xc4);
    emit8(generator, 0x49); emit8(generator, 0xd1); emit8(generator, 0xe6);

    define_label(generator, read_ready);
    emit8(generator, 0xb8); emit32(generator, 0);
    emit8(generator, 0x31); emit8(generator, 0xff);
    emit8(generator, 0x4b); emit8(generator, 0x8d);
    emit8(generator, 0x34); emit8(generator, 0x2c);
    emit8(generator, 0xba); emit32(generator, 1);
    emit_syscall(generator);
    emit8(generator, 0x48); emit8(generator, 0x83);
    emit8(generator, 0xf8); emit8(generator, 1);
    emit_conditional_jump(generator, 0x85, input_done);

    emit8(generator, 0x43); emit8(generator, 0x0f);
    emit8(generator, 0xb6); emit8(generator, 0x04); emit8(generator, 0x2c);
    emit8(generator, 0x3c); emit8(generator, '\n');
    emit_conditional_jump(generator, 0x84, line_done);
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xc5);
    emit_jump(generator, read_loop);

    define_label(generator, line_done);
    emit8(generator, 0x4d); emit8(generator, 0x85); emit8(generator, 0xed);
    emit_conditional_jump(generator, 0x84, terminate_line);
    emit8(generator, 0x43); emit8(generator, 0x0f);
    emit8(generator, 0xb6); emit8(generator, 0x44);
    emit8(generator, 0x2c); emit8(generator, 0xff);
    emit8(generator, 0x3c); emit8(generator, '\r');
    emit_conditional_jump(generator, 0x85, terminate_line);
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xcd);
    define_label(generator, terminate_line);
    emit8(generator, 0x43); emit8(generator, 0xc6);
    emit8(generator, 0x04); emit8(generator, 0x2c); emit8(generator, 0);
    emit8(generator, 0x4c); emit8(generator, 0x89); emit8(generator, 0xe0);
    emit_jump(generator, helper_done);

    define_label(generator, input_done);
    emit8(generator, 0x4d); emit8(generator, 0x85); emit8(generator, 0xed);
    emit_conditional_jump(generator, 0x85, line_done);
    define_label(generator, allocation_failed);
    emit8(generator, 0x31); emit8(generator, 0xc0);

    define_label(generator, helper_done);
    emit8(generator, 0x41); emit8(generator, 0x5e);
    emit8(generator, 0x41); emit8(generator, 0x5d);
    emit8(generator, 0x41); emit8(generator, 0x5c);
    emit8(generator, 0xc9);
    emit8(generator, 0xc3);
}

static void emit_read_key_helper(Generator *generator, size_t label)
{
    define_label(generator, label);
    emit8(generator, 0x55);
    emit8(generator, 0x48); emit8(generator, 0x89); emit8(generator, 0xe5);
    emit8(generator, 0x53);
    emit8(generator, 0x41); emit8(generator, 0x54);
    emit8(generator, 0x41); emit8(generator, 0x55);
    emit8(generator, 0x41); emit8(generator, 0x56);

    emit_mov_imm64(generator, REG_RAX, 9);
    emit_mov_imm64(generator, REG_RDI, 0);
    emit_mov_imm64(generator, REG_RSI, 4096);
    emit_mov_imm64(generator, REG_RDX, 3);
    emit_mov_imm64(generator, REG_R10, 0x22);
    emit_mov_imm64(generator, REG_R8, UINT64_MAX);
    emit_mov_imm64(generator, REG_R9, 0);
    emit_syscall(generator);
    emit_test_rax(generator);

    size_t allocation_failed = new_label(generator);
    size_t canonical_read = new_label(generator);
    size_t raw_read = new_label(generator);
    size_t restore_terminal = new_label(generator);
    size_t read_result = new_label(generator);
    size_t read_failed = new_label(generator);
    size_t done = new_label(generator);
    emit_conditional_jump(generator, 0x88, allocation_failed);
    emit_mov_reg_reg(generator, REG_R12, REG_RAX);

    emit_mov_imm64(generator, REG_RAX, 16);
    emit_mov_imm64(generator, REG_RDI, 0);
    emit_mov_imm64(generator, REG_RSI, 0x5401);
    emit_mov_reg_reg(generator, REG_RDX, REG_R12);
    emit_syscall(generator);
    emit_test_rax(generator);
    emit_conditional_jump(generator, 0x88, canonical_read);

    emit8(generator, 0x31); emit8(generator, 0xdb);
    emit8(generator, 0x45); emit8(generator, 0x8b);
    emit8(generator, 0x6c); emit8(generator, 0x24); emit8(generator, 0x0c);
    emit8(generator, 0x41); emit8(generator, 0x8a);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x16);
    emit8(generator, 0x41); emit8(generator, 0x88);
    emit8(generator, 0x84); emit8(generator, 0x24);
    emit32(generator, 128);
    emit8(generator, 0x41); emit8(generator, 0x8a);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x17);
    emit8(generator, 0x41); emit8(generator, 0x88);
    emit8(generator, 0x84); emit8(generator, 0x24);
    emit32(generator, 129);

    emit8(generator, 0x41); emit8(generator, 0x81);
    emit8(generator, 0x64); emit8(generator, 0x24); emit8(generator, 0x0c);
    emit32(generator, UINT32_C(0xfffffff5));
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x16);
    emit8(generator, 0);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x17);
    emit8(generator, 1);
    emit_mov_imm64(generator, REG_RAX, 16);
    emit_mov_imm64(generator, REG_RDI, 0);
    emit_mov_imm64(generator, REG_RSI, 0x5402);
    emit_mov_reg_reg(generator, REG_RDX, REG_R12);
    emit_syscall(generator);
    emit_test_rax(generator);
    emit_conditional_jump(generator, 0x88, canonical_read);
    emit8(generator, 0xbb); emit32(generator, 1);
    emit_jump(generator, raw_read);

    define_label(generator, canonical_read);
    emit8(generator, 0x31); emit8(generator, 0xdb);
    define_label(generator, raw_read);
    emit_mov_imm64(generator, REG_RAX, 0);
    emit_mov_imm64(generator, REG_RDI, 0);
    emit8(generator, 0x49); emit8(generator, 0x8d);
    emit8(generator, 0x74); emit8(generator, 0x24); emit8(generator, 0x40);
    emit_mov_imm64(generator, REG_RDX, 1);
    emit_syscall(generator);
    emit_mov_reg_reg(generator, REG_R14, REG_RAX);

    define_label(generator, restore_terminal);
    emit8(generator, 0x85); emit8(generator, 0xdb);
    emit_conditional_jump(generator, 0x84, read_result);
    emit8(generator, 0x45); emit8(generator, 0x89);
    emit8(generator, 0x6c); emit8(generator, 0x24); emit8(generator, 0x0c);
    emit8(generator, 0x41); emit8(generator, 0x8a);
    emit8(generator, 0x84); emit8(generator, 0x24);
    emit32(generator, 128);
    emit8(generator, 0x41); emit8(generator, 0x88);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x16);
    emit8(generator, 0x41); emit8(generator, 0x8a);
    emit8(generator, 0x84); emit8(generator, 0x24);
    emit32(generator, 129);
    emit8(generator, 0x41); emit8(generator, 0x88);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x17);
    emit_mov_imm64(generator, REG_RAX, 16);
    emit_mov_imm64(generator, REG_RDI, 0);
    emit_mov_imm64(generator, REG_RSI, 0x5402);
    emit_mov_reg_reg(generator, REG_RDX, REG_R12);
    emit_syscall(generator);

    define_label(generator, read_result);
    emit8(generator, 0x4d); emit8(generator, 0x85); emit8(generator, 0xf6);
    emit_conditional_jump(generator, 0x8e, read_failed);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x41);
    emit8(generator, 0);
    emit8(generator, 0x49); emit8(generator, 0x8d);
    emit8(generator, 0x44); emit8(generator, 0x24); emit8(generator, 0x40);
    emit_jump(generator, done);

    define_label(generator, read_failed);
    emit8(generator, 0x31); emit8(generator, 0xc0);

    define_label(generator, allocation_failed);
    emit8(generator, 0x31); emit8(generator, 0xc0);
    define_label(generator, done);
    emit8(generator, 0x41); emit8(generator, 0x5e);
    emit8(generator, 0x41); emit8(generator, 0x5d);
    emit8(generator, 0x41); emit8(generator, 0x5c);
    emit8(generator, 0x5b);
    emit8(generator, 0xc9);
    emit8(generator, 0xc3);
}

static size_t append_console_color_table(Generator *generator)
{
    static const unsigned char ansi_colors[] = {
        30, 34, 32, 36, 31, 35, 33, 37,
        90, 94, 92, 96, 91, 95, 93, 97
    };
    size_t offset = generator->data.size;
    for (size_t i = 0; i < sizeof(ansi_colors); i++)
        emit_data8(generator, ansi_colors[i]);
    return offset;
}

static void emit_console_color_helper(
    Generator *generator,
    size_t label,
    size_t color_table)
{
    define_label(generator, label);
    emit8(generator, 0x41); emit8(generator, 0x54);
    emit8(generator, 0x41); emit8(generator, 0x55);
    emit8(generator, 0x41); emit8(generator, 0x56);
    emit_stack_adjust(generator, 1, 32);

    emit_mov_reg_reg(generator, REG_R12, REG_RDI);
    emit_mov_reg_reg(generator, REG_R13, REG_RSI);
    size_t invalid = new_label(generator);
    emit8(generator, 0x49); emit8(generator, 0x83);
    emit8(generator, 0xfc); emit8(generator, 0);
    emit_conditional_jump(generator, 0x88, invalid);
    emit8(generator, 0x49); emit8(generator, 0x83);
    emit8(generator, 0xfc); emit8(generator, 15);
    emit_conditional_jump(generator, 0x87, invalid);

    emit_lea_rax_data(generator, color_table);
    emit8(generator, 0x4a); emit8(generator, 0x0f);
    emit8(generator, 0xb6); emit8(generator, 0x04); emit8(generator, 0x20);
    emit8(generator, 0x4d); emit8(generator, 0x85); emit8(generator, 0xed);
    size_t foreground = new_label(generator);
    emit_conditional_jump(generator, 0x84, foreground);
    emit8(generator, 0x48); emit8(generator, 0x83);
    emit8(generator, 0xc0); emit8(generator, 10);
    define_label(generator, foreground);
    emit_mov_reg_reg(generator, REG_R12, REG_RAX);

    emit8(generator, 0x4c); emit8(generator, 0x8d);
    emit8(generator, 0x74); emit8(generator, 0x24); emit8(generator, 0x1f);
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x06); emit8(generator, 'm');
    emit_mov_reg_reg(generator, REG_RAX, REG_R12);

    size_t digits = new_label(generator);
    define_label(generator, digits);
    emit8(generator, 0x48); emit8(generator, 0x31); emit8(generator, 0xd2);
    emit8(generator, 0x48); emit8(generator, 0xc7);
    emit8(generator, 0xc1); emit32(generator, 10);
    emit8(generator, 0x48); emit8(generator, 0xf7); emit8(generator, 0xf1);
    emit8(generator, 0x80); emit8(generator, 0xc2); emit8(generator, '0');
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0x88);
    emit8(generator, 0x16);
    emit_test_rax(generator);
    emit_conditional_jump(generator, 0x85, digits);

    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x06); emit8(generator, '[');
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x06); emit8(generator, 27);
    emit_mov_reg_reg(generator, REG_RSI, REG_R14);
    emit8(generator, 0x48); emit8(generator, 0x8d);
    emit8(generator, 0x54); emit8(generator, 0x24); emit8(generator, 31);
    emit8(generator, 0x4c); emit8(generator, 0x29); emit8(generator, 0xf2);
    emit_write_syscall(generator, 1);
    emit_stack_adjust(generator, 0, 32);
    emit8(generator, 0x41); emit8(generator, 0x5e);
    emit8(generator, 0x41); emit8(generator, 0x5d);
    emit8(generator, 0x41); emit8(generator, 0x5c);
    emit8(generator, 0xc3);

    define_label(generator, invalid);
    emit_mov_imm64(generator, REG_RAX, 60);
    emit_mov_imm64(generator, REG_RDI, 1);
    emit_syscall(generator);
}

static void emit_console_cursor_helper(Generator *generator, size_t label)
{
    define_label(generator, label);
    emit8(generator, 0x41); emit8(generator, 0x54);
    emit8(generator, 0x41); emit8(generator, 0x55);
    emit8(generator, 0x41); emit8(generator, 0x56);
    emit_stack_adjust(generator, 1, 64);
    emit_mov_reg_reg(generator, REG_R12, REG_RDI);
    emit_mov_reg_reg(generator, REG_R13, REG_RSI);

    size_t invalid_position = new_label(generator);
    emit8(generator, 0x4d); emit8(generator, 0x85); emit8(generator, 0xe4);
    emit_conditional_jump(generator, 0x88, invalid_position);
    emit8(generator, 0x4d); emit8(generator, 0x85); emit8(generator, 0xed);
    emit_conditional_jump(generator, 0x88, invalid_position);

    emit8(generator, 0x4c); emit8(generator, 0x8d);
    emit8(generator, 0x74); emit8(generator, 0x24); emit8(generator, 63);
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x06); emit8(generator, 'H');
    emit_mov_reg_reg(generator, REG_RAX, REG_R12);
    emit8(generator, 0x48); emit8(generator, 0xff); emit8(generator, 0xc0);
    size_t column_digits = new_label(generator);
    define_label(generator, column_digits);
    emit8(generator, 0x48); emit8(generator, 0x31); emit8(generator, 0xd2);
    emit8(generator, 0x48); emit8(generator, 0xc7);
    emit8(generator, 0xc1); emit32(generator, 10);
    emit8(generator, 0x48); emit8(generator, 0xf7); emit8(generator, 0xf1);
    emit8(generator, 0x80); emit8(generator, 0xc2); emit8(generator, '0');
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0x88);
    emit8(generator, 0x16);
    emit_test_rax(generator);
    emit_conditional_jump(generator, 0x85, column_digits);

    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x06); emit8(generator, ';');
    emit_mov_reg_reg(generator, REG_RAX, REG_R13);
    emit8(generator, 0x48); emit8(generator, 0xff); emit8(generator, 0xc0);
    size_t row_digits = new_label(generator);
    define_label(generator, row_digits);
    emit8(generator, 0x48); emit8(generator, 0x31); emit8(generator, 0xd2);
    emit8(generator, 0x48); emit8(generator, 0xc7);
    emit8(generator, 0xc1); emit32(generator, 10);
    emit8(generator, 0x48); emit8(generator, 0xf7); emit8(generator, 0xf1);
    emit8(generator, 0x80); emit8(generator, 0xc2); emit8(generator, '0');
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0x88);
    emit8(generator, 0x16);
    emit_test_rax(generator);
    emit_conditional_jump(generator, 0x85, row_digits);

    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x06); emit8(generator, '[');
    emit8(generator, 0x49); emit8(generator, 0xff); emit8(generator, 0xce);
    emit8(generator, 0x41); emit8(generator, 0xc6);
    emit8(generator, 0x06); emit8(generator, 27);
    emit_mov_reg_reg(generator, REG_RSI, REG_R14);
    emit8(generator, 0x48); emit8(generator, 0x8d);
    emit8(generator, 0x54); emit8(generator, 0x24); emit8(generator, 63);
    emit8(generator, 0x4c); emit8(generator, 0x29); emit8(generator, 0xf2);
    emit_write_syscall(generator, 1);
    emit_stack_adjust(generator, 0, 64);
    emit8(generator, 0x41); emit8(generator, 0x5e);
    emit8(generator, 0x41); emit8(generator, 0x5d);
    emit8(generator, 0x41); emit8(generator, 0x5c);
    emit8(generator, 0xc3);

    define_label(generator, invalid_position);
    emit_mov_imm64(generator, REG_RAX, 60);
    emit_mov_imm64(generator, REG_RDI, 1);
    emit_syscall(generator);
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
    size_t helper_read_line = new_label(&generator);
    size_t helper_read_key = new_label(&generator);
    size_t helper_console_color = new_label(&generator);
    size_t helper_console_cursor = new_label(&generator);
    size_t helper_string_native = new_label(&generator);
    size_t helper_array_native = new_label(&generator);
    size_t helper_convert_native = new_label(&generator);
    generator.function_labels = calloc(
        generator.statement_count + 10,
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
        generator.function_labels[generator.statement_count + 3] =
            helper_read_line;
        generator.function_labels[generator.statement_count + 4] =
            helper_read_key;
        generator.function_labels[generator.statement_count + 5] =
            helper_console_color;
        generator.function_labels[generator.statement_count + 6] =
            helper_console_cursor;
        generator.function_labels[generator.statement_count + 7] =
            helper_string_native;
        generator.function_labels[generator.statement_count + 8] =
            helper_array_native;
        generator.function_labels[generator.statement_count + 9] =
            helper_convert_native;
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
        emit_read_line_helper(&generator, helper_read_line);
    if (!generator.failed)
        emit_read_key_helper(&generator, helper_read_key);
    if (!generator.failed) {
        size_t color_table = append_console_color_table(&generator);
        emit_console_color_helper(
            &generator, helper_console_color, color_table);
    }
    if (!generator.failed)
        emit_console_cursor_helper(&generator, helper_console_cursor);
    if (!generator.failed) {
        define_label(&generator, helper_string_native);
        size_t runtime_size = (size_t)(
            _binary_string_native_bin_end - _binary_string_native_bin_start);
        for (size_t i = 0; i < runtime_size; i++)
            emit8(&generator, _binary_string_native_bin_start[i]);
    }
    if (!generator.failed) {
        define_label(&generator, helper_array_native);
        size_t runtime_size = (size_t)(
            _binary_array_native_bin_end - _binary_array_native_bin_start);
        for (size_t i = 0; i < runtime_size; i++)
            emit8(&generator, _binary_array_native_bin_start[i]);
    }
    if (!generator.failed) {
        while (generator.code.size % 16)
            emit8(&generator, 0x90);
        define_label(&generator, helper_convert_native);
        size_t runtime_size = (size_t)(
            _binary_convert_native_bin_end -
            _binary_convert_native_bin_start);
        for (size_t i = 0; i < runtime_size; i++)
            emit8(&generator, _binary_convert_native_bin_start[i]);
    }

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
