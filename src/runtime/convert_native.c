#include <stddef.h>
#include <stdint.h>

#include "runtime/convert_native.h"

#define SSHARP_NATIVE __attribute__((section(".text.ssharp_convert_native"), noinline, used))

static inline __attribute__((always_inline)) void fail_process(void)
{
    register long rax __asm__("rax") = 60;
    register long rdi __asm__("rdi") = 1;
    __asm__ volatile ("syscall" : "+a"(rax) : "D"(rdi) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static inline __attribute__((always_inline))
char *allocate_string(size_t length)
{
    if (length >= SIZE_MAX - 4095)
        return NULL;
    size_t size = (length + 1 + 4095) & ~(size_t)4095;
    register long r10 __asm__("r10") = 0x22;
    register long r8 __asm__("r8") = -1;
    register long r9 __asm__("r9") = 0;
    register long rax __asm__("rax") = 9;
    register long rdi __asm__("rdi") = 0;
    register long rsi __asm__("rsi") = (long)size;
    register long rdx __asm__("rdx") = 3;
    __asm__ volatile (
        "syscall"
        : "+a"(rax)
        : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    if ((long)rax < 0 && (long)rax >= -4095)
        return NULL;
    return (char *)(intptr_t)rax;
}

static inline __attribute__((always_inline))
size_t text_length(const char *text)
{
    size_t length = 0;
    if (text)
        while (text[length])
            length++;
    return length;
}

static inline __attribute__((always_inline))
int ascii_space(unsigned char ch)
{
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

static inline __attribute__((always_inline))
void append_unsigned(char *buffer, size_t *length, uint64_t value)
{
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        buffer[(*length)++] = digits[--count];
}

static inline __attribute__((always_inline))
char *integer_to_string(uint64_t value, int is_signed, int negative)
{
    char reverse[21];
    size_t length = 0;
    if (is_signed && negative)
        reverse[length++] = '-';
    uint64_t magnitude = negative ? 0 - value : value;
    do {
        reverse[length++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);

    char *result = allocate_string(length);
    if (!result)
        fail_process();
    size_t output = 0;
    if (is_signed && negative) {
        result[output++] = reverse[0];
        while (length > 1)
            result[output++] = reverse[--length];
    } else {
        while (length)
            result[output++] = reverse[--length];
    }
    result[output] = '\0';
    return result;
}

static inline __attribute__((always_inline))
int parse_integer(const char *text, int is_unsigned, uint64_t *result)
{
    if (!text)
        return 0;
    size_t length = text_length(text);
    size_t begin = 0;
    size_t end = length;
    while (begin < end && ascii_space((unsigned char)text[begin]))
        begin++;
    while (end > begin && ascii_space((unsigned char)text[end - 1]))
        end--;
    if (begin == end)
        return 0;

    int negative = 0;
    if (text[begin] == '+' || text[begin] == '-') {
        negative = text[begin] == '-';
        if (negative && is_unsigned)
            return 0;
        begin++;
        if (begin == end)
            return 0;
    }

    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 :
        (is_unsigned ? UINT64_MAX : INT64_MAX);
    uint64_t value = 0;
    for (size_t i = begin; i < end; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < '0' || ch > '9')
            return 0;
        unsigned digit = ch - '0';
        if (value > (limit - digit) / 10)
            return 0;
        value = value * 10 + digit;
    }
    *result = negative ? 0 - value : value;
    return 1;
}

static inline __attribute__((always_inline))
int parse_boolean(const char *text, int *value)
{
    if (!text)
        return 0;
    size_t begin = 0;
    size_t end = text_length(text);
    while (begin < end && ascii_space((unsigned char)text[begin]))
        begin++;
    while (end > begin && ascii_space((unsigned char)text[end - 1]))
        end--;
    size_t length = end - begin;
    if (length == 4 &&
        (text[begin] == 't' || text[begin] == 'T') &&
        (text[begin + 1] == 'r' || text[begin + 1] == 'R') &&
        (text[begin + 2] == 'u' || text[begin + 2] == 'U') &&
        (text[begin + 3] == 'e' || text[begin + 3] == 'E')) {
        *value = 1;
        return 1;
    }
    if (length == 5 &&
        (text[begin] == 'f' || text[begin] == 'F') &&
        (text[begin + 1] == 'a' || text[begin + 1] == 'A') &&
        (text[begin + 2] == 'l' || text[begin + 2] == 'L') &&
        (text[begin + 3] == 's' || text[begin + 3] == 'S') &&
        (text[begin + 4] == 'e' || text[begin + 4] == 'E')) {
        *value = 0;
        return 1;
    }
    return 0;
}

static inline __attribute__((always_inline))
int decode_utf8(const unsigned char *text, size_t length, uint32_t *value)
{
    if (!text || length == 0)
        return 0;
    unsigned char first = text[0];
    size_t count;
    uint32_t codepoint;
    if (first < 0x80) {
        count = 1;
        codepoint = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
        count = 2;
        codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
        count = 3;
        codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
        count = 4;
        codepoint = first & 0x07;
    } else {
        return 0;
    }
    if (length != count)
        return 0;
    for (size_t i = 1; i < count; i++) {
        if ((text[i] & 0xc0) != 0x80)
            return 0;
        codepoint = (codepoint << 6) | (text[i] & 0x3f);
    }
    if ((count == 2 && codepoint < 0x80) ||
        (count == 3 && codepoint < 0x800) ||
        (count == 4 && codepoint < 0x10000) ||
        codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return 0;
    *value = codepoint;
    return 1;
}

static inline __attribute__((always_inline))
double bits_to_double(uint64_t bits)
{
    union {
        uint64_t bits;
        double value;
    } converted = { .bits = bits };
    return converted.value;
}

static inline __attribute__((always_inline))
uint64_t double_to_bits(double value)
{
    union {
        uint64_t bits;
        double value;
    } converted = { .value = value };
    return converted.bits;
}

static inline __attribute__((always_inline))
double parse_double(const char *text, int *valid)
{
    if (!text) {
        *valid = 0;
        return 0;
    }
    size_t begin = 0;
    size_t end = text_length(text);
    while (begin < end && ascii_space((unsigned char)text[begin]))
        begin++;
    while (end > begin && ascii_space((unsigned char)text[end - 1]))
        end--;
    if (begin == end) {
        *valid = 0;
        return 0;
    }
    if (end - begin == 3 &&
        (text[begin] == 'n' || text[begin] == 'N') &&
        (text[begin + 1] == 'a' || text[begin + 1] == 'A') &&
        (text[begin + 2] == 'n' || text[begin + 2] == 'N')) {
        *valid = 1;
        return bits_to_double(UINT64_C(0x7ff8000000000000));
    }
    size_t infinity_word = begin;
    int infinity_negative = 0;
    if (text[infinity_word] == '+' || text[infinity_word] == '-') {
        infinity_negative = text[infinity_word] == '-';
        infinity_word++;
    }
    if (end - infinity_word == 8) {
        size_t word = infinity_word;
        if (
            (text[word] == 'i' || text[word] == 'I') &&
            (text[word + 1] == 'n' || text[word + 1] == 'N') &&
            (text[word + 2] == 'f' || text[word + 2] == 'F') &&
            (text[word + 3] == 'i' || text[word + 3] == 'I') &&
            (text[word + 4] == 'n' || text[word + 4] == 'N') &&
            (text[word + 5] == 'i' || text[word + 5] == 'I') &&
            (text[word + 6] == 't' || text[word + 6] == 'T') &&
            (text[word + 7] == 'y' || text[word + 7] == 'Y')) {
            *valid = 1;
            uint64_t infinity = UINT64_C(0x7ff0000000000000);
            return bits_to_double(infinity |
                (infinity_negative
                    ? UINT64_C(0x8000000000000000)
                    : 0));
        }
    }

    int negative = 0;
    if (text[begin] == '+' || text[begin] == '-') {
        negative = text[begin] == '-';
        begin++;
    }
    double value = 0;
    int has_digit = 0;
    int fractional_digits = 0;
    int after_decimal = 0;
    size_t i = begin;
    for (; i < end; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '.' && !after_decimal) {
            after_decimal = 1;
            continue;
        }
        if (ch < '0' || ch > '9')
            break;
        value = value * (double)10 + (double)(ch - '0');
        has_digit = 1;
        if (after_decimal && fractional_digits < 1000)
            fractional_digits++;
    }
    if (!has_digit) {
        *valid = 0;
        return 0;
    }

    int exponent = -fractional_digits;
    if (i < end && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        int exponent_negative = 0;
        if (i < end && (text[i] == '+' || text[i] == '-')) {
            exponent_negative = text[i] == '-';
            i++;
        }
        int exponent_digits = 0;
        int parsed_exponent = 0;
        while (i < end && text[i] >= '0' && text[i] <= '9') {
            if (parsed_exponent < 1000)
                parsed_exponent = parsed_exponent * 10 + text[i] - '0';
            exponent_digits++;
            i++;
        }
        if (!exponent_digits) {
            *valid = 0;
            return 0;
        }
        exponent += exponent_negative ? -parsed_exponent : parsed_exponent;
    }
    if (i != end) {
        *valid = 0;
        return 0;
    }

    double ten = bits_to_double(UINT64_C(0x4024000000000000));
    if (exponent > 308) {
        value = bits_to_double(UINT64_C(0x7ff0000000000000));
    } else if (exponent < -400) {
        value = 0;
    } else if (exponent > 0) {
        for (int n = 0; n < exponent; n++)
            value *= ten;
    } else {
        for (int n = 0; n > exponent; n--)
            value /= ten;
    }
    *valid = 1;
    return negative ? -value : value;
}

static inline __attribute__((always_inline))
char *double_to_string(double value)
{
    uint64_t bits = double_to_bits(value);
    int negative = (bits >> 63) != 0;
    uint64_t exponent = (bits >> 52) & 0x7ff;
    uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);
    char *result = allocate_string(400);
    if (!result)
        fail_process();
    size_t output = 0;
    if (exponent == 0x7ff) {
        if (fraction) {
            result[output++] = 'N';
            result[output++] = 'a';
            result[output++] = 'N';
        } else {
            if (negative)
                result[output++] = '-';
            result[output++] = 'I';
            result[output++] = 'n';
            result[output++] = 'f';
            result[output++] = 'i';
            result[output++] = 'n';
            result[output++] = 'i';
            result[output++] = 't';
            result[output++] = 'y';
        }
        result[output] = '\0';
        return result;
    }

    if (negative)
        result[output++] = '-';
    double magnitude = negative ? -value : value;
    double large_value_threshold =
        bits_to_double(UINT64_C(0x43abc16d674ec800));
    double small_value_threshold =
        bits_to_double(UINT64_C(0x3e112e0be826d695));
    if (magnitude > large_value_threshold ||
        (magnitude != 0 && magnitude < small_value_threshold)) {
        int exponent10 = 0;
        double normalized = magnitude;
        double ten = bits_to_double(UINT64_C(0x4024000000000000));
        if (normalized >= ten) {
            while (normalized >= ten) {
                normalized /= ten;
                exponent10++;
            }
        } else {
            while (normalized < 1) {
                normalized *= ten;
                exponent10--;
            }
        }
        unsigned first_digit = (unsigned)normalized;
        result[output++] = (char)('0' + first_digit);
        double fraction_value = normalized - first_digit;
        size_t fractional_start = output;
        result[output++] = '.';
        for (size_t i = 0; i < 8; i++) {
            fraction_value *= ten;
            unsigned digit = (unsigned)fraction_value;
            if (digit > 9)
                digit = 9;
            result[output++] = (char)('0' + digit);
            fraction_value -= digit;
        }
        while (output > fractional_start + 1 &&
               result[output - 1] == '0')
            output--;
        if (result[output - 1] == '.')
            output--;
        result[output++] = 'E';
        if (exponent10 >= 0)
            result[output++] = '+';
        else {
            result[output++] = '-';
            exponent10 = -exponent10;
        }
        append_unsigned(result, &output, (uint64_t)exponent10);
    } else {
        uint64_t integer = (uint64_t)magnitude;
        append_unsigned(result, &output, integer);
        double fraction_value = magnitude - (double)integer;
        if (fraction_value > 0) {
            result[output++] = '.';
            for (size_t i = 0; i < 9; i++) {
                fraction_value *= (double)10;
                unsigned digit = (unsigned)fraction_value;
                if (digit > 9)
                    digit = 9;
                result[output++] = (char)('0' + digit);
                fraction_value -= digit;
                if (fraction_value == 0)
                    break;
            }
            while (result[output - 1] == '0')
                output--;
            if (result[output - 1] == '.')
                output--;
        }
    }
    result[output] = '\0';
    return result;
}

static inline __attribute__((always_inline))
char *char_to_string(uint32_t value)
{
    size_t length = value < 0x80 ? 1 :
        (value < 0x800 ? 2 : (value < 0x10000 ? 3 : 4));
    char *result = allocate_string(length);
    if (!result)
        fail_process();
    if (length == 1) {
        result[0] = (char)value;
    } else if (length == 2) {
        result[0] = (char)(0xc0 | (value >> 6));
        result[1] = (char)(0x80 | (value & 0x3f));
    } else if (length == 3) {
        result[0] = (char)(0xe0 | (value >> 12));
        result[1] = (char)(0x80 | ((value >> 6) & 0x3f));
        result[2] = (char)(0x80 | (value & 0x3f));
    } else {
        result[0] = (char)(0xf0 | (value >> 18));
        result[1] = (char)(0x80 | ((value >> 12) & 0x3f));
        result[2] = (char)(0x80 | ((value >> 6) & 0x3f));
        result[3] = (char)(0x80 | (value & 0x3f));
    }
    result[length] = '\0';
    return result;
}

SSHARP_NATIVE
long long ssharp_convert_native(
    long long raw_value,
    long long source_type,
    long long unused1,
    long long unused2,
    long long unused3,
    long long operation)
{
    (void)unused1;
    (void)unused2;
    (void)unused3;
    uint64_t bits = (uint64_t)raw_value;
    int is_null = source_type == SSHARP_CONVERT_NULL ||
        (source_type == SSHARP_CONVERT_STRING && !raw_value);
    int is_numeric_float = source_type == SSHARP_CONVERT_FLOAT ||
        source_type == SSHARP_CONVERT_DOUBLE;
    double floating_value = is_numeric_float
        ? (source_type == SSHARP_CONVERT_FLOAT
            ? (double)(float)(union { uint32_t bits; float value; })
                { .bits = (uint32_t)bits }.value
            : bits_to_double(bits))
        : 0;

    if (operation == SSHARP_CONVERT_TO_STRING) {
        if (is_null) {
            char *empty = allocate_string(0);
            if (!empty)
                fail_process();
            return (long long)(intptr_t)empty;
        }
        if (source_type == SSHARP_CONVERT_STRING)
            return raw_value;
        if (source_type == SSHARP_CONVERT_BOOL) {
            char *result = allocate_string(bits ? 4 : 5);
            if (!result)
                fail_process();
            if (bits) {
                result[0] = 'T'; result[1] = 'r';
                result[2] = 'u'; result[3] = 'e'; result[4] = '\0';
            } else {
                result[0] = 'F'; result[1] = 'a'; result[2] = 'l';
                result[3] = 's'; result[4] = 'e'; result[5] = '\0';
            }
            return (long long)(intptr_t)result;
        }
        if (source_type == SSHARP_CONVERT_CHAR)
            return (long long)(intptr_t)char_to_string((uint32_t)bits);
        if (is_numeric_float)
            return (long long)(intptr_t)double_to_string(floating_value);
        if (source_type == SSHARP_CONVERT_UINT64)
            return (long long)(intptr_t)integer_to_string(bits, 0, 0);
        int64_t signed_value = (int64_t)bits;
        return (long long)(intptr_t)integer_to_string(
            bits, 1, signed_value < 0);
    }

    if (operation == SSHARP_CONVERT_TO_BOOLEAN) {
        if (is_null)
            return 0;
        if (source_type == SSHARP_CONVERT_STRING) {
            int boolean;
            if (!parse_boolean((const char *)(intptr_t)raw_value, &boolean))
                fail_process();
            return boolean;
        }
        if (source_type == SSHARP_CONVERT_BOOL)
            return bits != 0;
        if (is_numeric_float)
            return floating_value != 0;
        return bits != 0;
    }

    if (operation == SSHARP_CONVERT_TO_CHAR) {
        uint32_t codepoint;
        if (is_null)
            return 0;
        if (source_type == SSHARP_CONVERT_STRING) {
            if (!decode_utf8((const unsigned char *)(intptr_t)raw_value,
                    text_length((const char *)(intptr_t)raw_value),
                    &codepoint) || codepoint > 0xffff)
                fail_process();
            return codepoint;
        }
        if (is_numeric_float) {
            if (floating_value < 0 || floating_value > 0xffff)
                fail_process();
            codepoint = (uint32_t)floating_value;
        } else {
            if (source_type == SSHARP_CONVERT_UINT64) {
                if (bits > 0xffff)
                    fail_process();
                codepoint = (uint32_t)bits;
            } else {
                int64_t signed_value = (int64_t)bits;
                if (signed_value < 0 || signed_value > 0xffff)
                    fail_process();
                codepoint = (uint32_t)signed_value;
            }
        }
        if (codepoint >= 0xd800 && codepoint <= 0xdfff)
            fail_process();
        return codepoint;
    }

    if (operation == SSHARP_CONVERT_TO_SINGLE ||
        operation == SSHARP_CONVERT_TO_DOUBLE ||
        operation == SSHARP_CONVERT_TO_DECIMAL) {
        double value = 0;
        if (is_null) {
            value = 0;
        } else if (source_type == SSHARP_CONVERT_STRING) {
            int valid;
            value = parse_double(
                (const char *)(intptr_t)raw_value, &valid);
            if (!valid)
                fail_process();
        } else if (source_type == SSHARP_CONVERT_BOOL) {
            value = (double)(bits != 0);
        } else if (is_numeric_float) {
            value = floating_value;
        } else if (source_type == SSHARP_CONVERT_UINT64) {
            value = (double)bits;
        } else {
            value = (double)(int64_t)bits;
        }

        if (operation == SSHARP_CONVERT_TO_SINGLE) {
            float converted = (float)value;
            union { uint32_t bits; float value; } result = { .value = converted };
            if (value == value && converted != converted)
                fail_process();
            return (long long)result.bits;
        }
        return (long long)double_to_bits(value);
    }

    if (is_null)
        bits = 0;
    else if (source_type == SSHARP_CONVERT_STRING) {
        if (!parse_integer((const char *)(intptr_t)raw_value,
                operation == SSHARP_CONVERT_TO_UINT64, &bits))
            fail_process();
    } else if (source_type == SSHARP_CONVERT_BOOL) {
        bits = bits != 0;
    }

    uint64_t minimum = 0;
    uint64_t maximum = UINT64_MAX;
    int unsigned_result = 0;
    switch (operation) {
        case SSHARP_CONVERT_TO_BYTE:
            maximum = UINT8_MAX;
            unsigned_result = 1;
            break;
        case SSHARP_CONVERT_TO_SBYTE:
            minimum = (uint64_t)(int64_t)INT8_MIN;
            maximum = INT8_MAX;
            break;
        case SSHARP_CONVERT_TO_INT16:
            minimum = (uint64_t)(int64_t)INT16_MIN;
            maximum = INT16_MAX;
            break;
        case SSHARP_CONVERT_TO_UINT16:
            maximum = UINT16_MAX;
            unsigned_result = 1;
            break;
        case SSHARP_CONVERT_TO_INT32:
            minimum = (uint64_t)(int64_t)INT32_MIN;
            maximum = INT32_MAX;
            break;
        case SSHARP_CONVERT_TO_UINT32:
            maximum = UINT32_MAX;
            unsigned_result = 1;
            break;
        case SSHARP_CONVERT_TO_INT64:
            minimum = (uint64_t)INT64_MIN;
            maximum = INT64_MAX;
            break;
        case SSHARP_CONVERT_TO_UINT64:
            unsigned_result = 1;
            break;
        default:
            fail_process();
    }

    if (is_numeric_float) {
        if (floating_value != floating_value)
            fail_process();
        int negative = floating_value < 0;
        double magnitude = negative ? -floating_value : floating_value;
        double limit = unsigned_result
            ? bits_to_double(UINT64_C(0x43f0000000000000))
            : bits_to_double(UINT64_C(0x43e0000000000000));
        if ((unsigned_result && negative) ||
            (negative ? magnitude > limit : magnitude >= limit))
            fail_process();

        uint64_t rounded = (uint64_t)magnitude;
        double fraction = magnitude - (double)rounded;
        if (fraction > 0.5 ||
            (fraction == 0.5 && (rounded & UINT64_C(1))))
            rounded++;
        if (!unsigned_result && negative)
            bits = 0 - rounded;
        else
            bits = rounded;
    }

    if (unsigned_result) {
        if (source_type == SSHARP_CONVERT_INT &&
            (int64_t)bits < 0)
            fail_process();
        if (source_type == SSHARP_CONVERT_CHAR &&
            (int64_t)bits < 0)
            fail_process();
        if (bits > maximum)
            fail_process();
    } else {
        if (source_type == SSHARP_CONVERT_UINT64 &&
            bits > (uint64_t)INT64_MAX)
            fail_process();
        int64_t signed_value = (int64_t)bits;
        if (signed_value < (int64_t)minimum ||
            signed_value > (int64_t)maximum)
            fail_process();
    }
    return (long long)bits;
}
