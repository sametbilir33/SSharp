#include <stddef.h>
#include <stdint.h>

#include "runtime/string_native.h"

#define SSHARP_NATIVE __attribute__((section(".text.ssharp_string_native"), noinline, used))

static inline __attribute__((always_inline))
size_t string_length(const char *text)
{
    size_t length = 0;
    while (text[length] != '\0')
        length++;
    return length;
}

static inline __attribute__((always_inline)) int string_match_at(
    const char *text,
    const char *needle,
    size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (text[i] != needle[i])
            return 0;
    }
    return 1;
}

static inline __attribute__((always_inline))
int is_ascii_space(unsigned char value)
{
    return value == ' ' || value == '\t' || value == '\n' ||
        value == '\r' || value == '\f' || value == '\v';
}

static inline __attribute__((always_inline))
char *allocate_string(size_t length)
{
    if (length >= SIZE_MAX - 4095)
        return NULL;
    size_t size = (length + 1 + 4095) & ~(size_t)4095;
    long result;
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
    result = rax;
    if (result < 0 && result >= -4095)
        return NULL;
    return (char *)result;
}

static inline __attribute__((always_inline)) void fail_process(void)
{
    register long rax __asm__("rax") = 60;
    register long rdi __asm__("rdi") = 1;
    __asm__ volatile ("syscall" : "+a"(rax) : "D"(rdi) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

SSHARP_NATIVE
long long ssharp_string_native(
    const char *source,
    const char *first,
    const char *second,
    long long start,
    long long count,
    long long operation)
{
    if (operation == SSHARP_STRING_CONCAT) {
        size_t source_length = source ? string_length(source) : 0;
        size_t addition_length = first ? string_length(first) : 0;
        if (addition_length > SIZE_MAX - source_length - 1)
            fail_process();
        char *result = allocate_string(source_length + addition_length);
        if (!result)
            fail_process();
        for (size_t i = 0; i < source_length; i++)
            result[i] = source[i];
        for (size_t i = 0; i < addition_length; i++)
            result[source_length + i] = first[i];
        result[source_length + addition_length] = '\0';
        return (long long)(intptr_t)result;
    }

    if (operation == SSHARP_STRING_IS_NULL_OR_EMPTY)
        return !source || source[0] == '\0';

    if (operation == SSHARP_STRING_IS_NULL_OR_WHITE_SPACE) {
        if (!source)
            return 1;
        while (*source) {
            if (!is_ascii_space((unsigned char)*source))
                return 0;
            source++;
        }
        return 1;
    }

    if (!source)
        fail_process();

    if (operation == SSHARP_STRING_LENGTH)
        return (long long)string_length(source);

    if (operation == SSHARP_STRING_CONTAINS ||
        operation == SSHARP_STRING_STARTS_WITH ||
        operation == SSHARP_STRING_ENDS_WITH ||
        operation == SSHARP_STRING_INDEX_OF ||
        operation == SSHARP_STRING_LAST_INDEX_OF) {
        if (!first)
            fail_process();
        size_t source_length = string_length(source);
        size_t needle_length = string_length(first);
        if (needle_length > source_length)
            return operation == SSHARP_STRING_CONTAINS ||
                operation == SSHARP_STRING_STARTS_WITH ||
                operation == SSHARP_STRING_ENDS_WITH ? 0 : -1;

        if (operation == SSHARP_STRING_STARTS_WITH)
            return string_match_at(source, first, needle_length);
        if (operation == SSHARP_STRING_ENDS_WITH)
            return string_match_at(
                source + source_length - needle_length, first, needle_length);

        long long found = -1;
        size_t last = source_length - needle_length;
        for (size_t i = 0; i <= last; i++) {
            if (string_match_at(source + i, first, needle_length)) {
                found = (long long)i;
                if (operation != SSHARP_STRING_LAST_INDEX_OF)
                    break;
            }
        }
        if (operation == SSHARP_STRING_CONTAINS)
            return found >= 0;
        return found;
    }

    if (operation == SSHARP_STRING_SUBSTRING) {
        size_t source_length = string_length(source);
        if (start < 0 || (uint64_t)start > source_length)
            fail_process();
        if (count == -1)
            count = (long long)(source_length - (size_t)start);
        if (count < 0 ||
            (uint64_t)count > source_length - (size_t)start)
            fail_process();
        char *result = allocate_string((size_t)count);
        if (!result)
            fail_process();
        for (size_t i = 0; i < (size_t)count; i++)
            result[i] = source[(size_t)start + i];
        result[count] = '\0';
        return (long long)(intptr_t)result;
    }

    if (operation == SSHARP_STRING_REPLACE) {
        if (!first || !second)
            fail_process();
        size_t source_length = string_length(source);
        size_t old_length = string_length(first);
        size_t new_length = string_length(second);
        if (old_length == 0)
            fail_process();
        size_t matches = 0;
        for (size_t i = 0; i + old_length <= source_length;) {
            if (string_match_at(source + i, first, old_length)) {
                matches++;
                i += old_length;
            } else {
                i++;
            }
        }
        if (new_length > old_length &&
            matches > (SIZE_MAX - source_length - 1) /
                (new_length - old_length))
            fail_process();
        size_t result_length = new_length >= old_length
            ? source_length + matches * (new_length - old_length)
            : source_length - matches * (old_length - new_length);
        char *result = allocate_string(result_length);
        if (!result)
            fail_process();
        size_t input = 0;
        size_t output = 0;
        while (input < source_length) {
            if (input + old_length <= source_length &&
                string_match_at(source + input, first, old_length)) {
                for (size_t i = 0; i < new_length; i++)
                    result[output++] = second[i];
                input += old_length;
            } else {
                result[output++] = source[input++];
            }
        }
        result[output] = '\0';
        return (long long)(intptr_t)result;
    }

    if (operation == SSHARP_STRING_FORMAT) {
        size_t format_length = string_length(source);
        size_t output_capacity = format_length;
        size_t argument_count = (size_t)start;
        const uint64_t *arguments = (const uint64_t *)first;
        for (size_t i = 0; i < argument_count; i++) {
            const uint64_t *argument = arguments - i * 2;
            uint64_t type = argument[0];
            uint64_t value = argument[1];
            size_t value_length;
            if (type == 1) {
                value_length = value
                    ? string_length((const char *)(intptr_t)value)
                    : 0;
            } else if (type == 2) {
                value_length = 20;
            } else if (type == 3) {
                value_length = value ? 4 : 5;
            } else {
                fail_process();
            }
            if (output_capacity == SIZE_MAX ||
                value_length > SIZE_MAX - output_capacity - 1)
                fail_process();
            output_capacity += value_length;
        }

        char *result = allocate_string(output_capacity);
        if (!result)
            fail_process();
        size_t input = 0;
        size_t output = 0;
        while (input < format_length) {
            if (source[input] == '{') {
                if (input + 1 < format_length && source[input + 1] == '{') {
                    result[output++] = '{';
                    input += 2;
                    continue;
                }
                input++;
                if (input >= format_length || source[input] < '0' ||
                    source[input] > '9')
                    fail_process();
                size_t index = 0;
                while (input < format_length &&
                    source[input] >= '0' && source[input] <= '9') {
                    size_t digit = (size_t)(source[input++] - '0');
                    if (index > (SIZE_MAX - digit) / 10)
                        fail_process();
                    index = index * 10 + digit;
                }
                if (input >= format_length || source[input++] != '}' ||
                    index >= argument_count)
                    fail_process();

                const uint64_t *argument = arguments - index * 2;
                uint64_t type = argument[0];
                uint64_t value = argument[1];
                if (type == 1) {
                    const char *text = (const char *)(intptr_t)value;
                    if (text) {
                        while (*text)
                            result[output++] = *text++;
                    }
                } else if (type == 3) {
                    if (value) {
                        result[output++] = 't';
                        result[output++] = 'r';
                        result[output++] = 'u';
                        result[output++] = 'e';
                    } else {
                        result[output++] = 'f';
                        result[output++] = 'a';
                        result[output++] = 'l';
                        result[output++] = 's';
                        result[output++] = 'e';
                    }
                } else {
                    long long number = (long long)value;
                    uint64_t magnitude = number < 0
                        ? 0 - (uint64_t)number
                        : (uint64_t)number;
                    char digits[20];
                    size_t digit_count = 0;
                    do {
                        digits[digit_count++] =
                            (char)('0' + magnitude % 10);
                        magnitude /= 10;
                    } while (magnitude);
                    if (number < 0)
                        result[output++] = '-';
                    while (digit_count)
                        result[output++] = digits[--digit_count];
                }
                continue;
            }
            if (source[input] == '}') {
                if (input + 1 >= format_length ||
                    source[input + 1] != '}')
                    fail_process();
                result[output++] = '}';
                input += 2;
                continue;
            }
            result[output++] = source[input++];
        }
        result[output] = '\0';
        return (long long)(intptr_t)result;
    }

    if (operation >= SSHARP_STRING_TRIM &&
        operation <= SSHARP_STRING_TO_LOWER) {
        size_t length = string_length(source);
        size_t begin = 0;
        size_t end = length;
        if (operation == SSHARP_STRING_TRIM ||
            operation == SSHARP_STRING_TRIM_START) {
            while (begin < end && is_ascii_space((unsigned char)source[begin]))
                begin++;
        }
        if (operation == SSHARP_STRING_TRIM ||
            operation == SSHARP_STRING_TRIM_END) {
            while (end > begin && is_ascii_space((unsigned char)source[end - 1]))
                end--;
        }
        size_t result_length = end - begin;
        char *result = allocate_string(result_length);
        if (!result)
            fail_process();
        for (size_t i = 0; i < result_length; i++) {
            unsigned char value = (unsigned char)source[begin + i];
            if (operation == SSHARP_STRING_TO_UPPER &&
                value >= 'a' && value <= 'z')
                value = (unsigned char)(value - ('a' - 'A'));
            else if (operation == SSHARP_STRING_TO_LOWER &&
                value >= 'A' && value <= 'Z')
                value = (unsigned char)(value + ('a' - 'A'));
            result[i] = (char)value;
        }
        result[result_length] = '\0';
        return (long long)(intptr_t)result;
    }

    fail_process();
    return 0;
}
