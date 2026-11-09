#include <stddef.h>
#include <stdint.h>

#include "runtime/array_native.h"

#define SSHARP_NATIVE __attribute__((section(".text.ssharp_array_native"), noinline, used))

static inline __attribute__((always_inline)) void fail_process(void)
{
    register long rax __asm__("rax") = 60;
    register long rdi __asm__("rdi") = 1;
    __asm__ volatile ("syscall" : "+a"(rax) : "D"(rdi) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static inline __attribute__((always_inline))
int compare_strings(const char *left, const char *right)
{
    if (!left || !right)
        return left == right ? 0 : (left ? 1 : -1);
    for (;;) {
        unsigned char left_value = (unsigned char)*left;
        unsigned char right_value = (unsigned char)*right;
        if (left_value != right_value)
            return left_value < right_value ? -1 : 1;
        if (left_value == 0)
            return 0;
        left++;
        right++;
    }
}

static inline __attribute__((always_inline))
int compare_values(uint64_t left, uint64_t right, long long operation)
{
    if (operation == SSHARP_ARRAY_SORT_STRING)
        return compare_strings((const char *)(intptr_t)left,
            (const char *)(intptr_t)right);
    if (operation == SSHARP_ARRAY_SORT_BOOL) {
        left = left != 0;
        right = right != 0;
    }
    return left < right ? -1 : (left > right ? 1 : 0);
}

static inline __attribute__((always_inline))
uint64_t *allocate_array(size_t length)
{
    if (length > (SIZE_MAX / sizeof(uint64_t)) - 1)
        return NULL;
    size_t bytes = (length + 1) * sizeof(uint64_t);
    if (bytes > SIZE_MAX - 4095)
        return NULL;
    size_t mapping_size = (bytes + 4095) & ~(size_t)4095;
    register long r10 __asm__("r10") = 0x22;
    register long r8 __asm__("r8") = -1;
    register long r9 __asm__("r9") = 0;
    register long rax __asm__("rax") = 9;
    register long rdi __asm__("rdi") = 0;
    register long rsi __asm__("rsi") = (long)mapping_size;
    register long rdx __asm__("rdx") = 3;
    __asm__ volatile (
        "syscall"
        : "+a"(rax)
        : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    if ((long)rax < 0 && (long)rax >= -4095)
        return NULL;
    uint64_t *array = (uint64_t *)(intptr_t)rax;
    array[0] = length;
    return array;
}

SSHARP_NATIVE
long long ssharp_array_native(
    long long array_value,
    long long index_value,
    long long element_value,
    long long length_value,
    long long unused,
    long long operation)
{
    (void)unused;
    if (operation == SSHARP_ARRAY_ALLOCATE) {
        if (length_value < 0)
            fail_process();
        uint64_t *result = allocate_array((size_t)length_value);
        if (!result)
            fail_process();
        return (long long)(intptr_t)result;
    }

    uint64_t *array = (uint64_t *)(intptr_t)array_value;
    if (!array)
        fail_process();
    if (operation == SSHARP_ARRAY_LENGTH)
        return (long long)array[0];

    size_t length = (size_t)array[0];
    if (operation == SSHARP_ARRAY_REVERSE) {
        for (size_t i = 0; i < length / 2; i++) {
            uint64_t temp = array[i + 1];
            array[i + 1] = array[length - i];
            array[length - i] = temp;
        }
        return array_value;
    }

    if (operation == SSHARP_ARRAY_SORT_INT ||
        operation == SSHARP_ARRAY_SORT_BOOL ||
        operation == SSHARP_ARRAY_SORT_STRING) {
        for (size_t i = 1; i < length; i++) {
            uint64_t value = array[i + 1];
            size_t j = i;
            while (j > 0 &&
                compare_values(array[j], value, operation) > 0) {
                array[j + 1] = array[j];
                j--;
            }
            array[j + 1] = value;
        }
        return array_value;
    }

    if (operation >= SSHARP_ARRAY_INDEX_OF_INT &&
        operation <= SSHARP_ARRAY_LAST_INDEX_OF_STRING) {
        long long found = -1;
        int last_index = operation >= SSHARP_ARRAY_LAST_INDEX_OF_INT;
        long long kind = last_index
            ? operation - SSHARP_ARRAY_LAST_INDEX_OF_INT
            : operation - SSHARP_ARRAY_INDEX_OF_INT;
        for (size_t i = 0; i < length; i++) {
            int equal = kind == 2
                ? compare_strings((const char *)(intptr_t)array[i + 1],
                    (const char *)(intptr_t)element_value) == 0
                : array[i + 1] == (uint64_t)element_value;
            if (equal) {
                found = (long long)i;
                if (!last_index)
                    break;
            }
        }
        return found;
    }

    if (operation == SSHARP_ARRAY_RESIZE) {
        if (length_value < 0)
            fail_process();
        uint64_t *resized = allocate_array((size_t)length_value);
        if (!resized)
            fail_process();
        size_t copied = length < (size_t)length_value
            ? length
            : (size_t)length_value;
        for (size_t i = 0; i < copied; i++)
            resized[i + 1] = array[i + 1];
        return (long long)(intptr_t)resized;
    }

    if (operation == SSHARP_ARRAY_COPY) {
        uint64_t *destination = (uint64_t *)(intptr_t)element_value;
        long long destination_index = length_value;
        long long count = unused;
        if (!destination || index_value < 0 || destination_index < 0 ||
            count < 0 || (uint64_t)index_value > array[0] ||
            (uint64_t)count > array[0] - (uint64_t)index_value ||
            (uint64_t)destination_index > destination[0] ||
            (uint64_t)count > destination[0] - (uint64_t)destination_index)
            fail_process();
        if (array == destination &&
            destination_index > index_value &&
            destination_index < index_value + count) {
            for (long long i = count; i > 0; i--)
                destination[destination_index + i] =
                    array[index_value + i];
        } else {
            for (long long i = 0; i < count; i++)
                destination[destination_index + i + 1] =
                    array[index_value + i + 1];
        }
        return 0;
    }

    if (operation == SSHARP_ARRAY_CLEAR ||
        operation == SSHARP_ARRAY_FILL ||
        operation == SSHARP_ARRAY_FILL_ALL) {
        long long start = index_value;
        long long count = length_value;
        uint64_t value = operation == SSHARP_ARRAY_CLEAR
            ? 0
            : (uint64_t)element_value;
        if (operation == SSHARP_ARRAY_FILL_ALL) {
            start = 0;
            count = (long long)array[0];
        }
        if (start < 0 || count < 0 ||
            (uint64_t)start > array[0] ||
            (uint64_t)count > array[0] - (uint64_t)start)
            fail_process();
        for (long long i = 0; i < count; i++)
            array[(size_t)(start + i) + 1] = value;
        return 0;
    }

    if (index_value < 0 || (uint64_t)index_value >= array[0])
        fail_process();
    if (operation == SSHARP_ARRAY_GET)
        return (long long)array[(size_t)index_value + 1];
    if (operation == SSHARP_ARRAY_SET) {
        array[(size_t)index_value + 1] = (uint64_t)element_value;
        return element_value;
    }
    fail_process();
    return 0;
}
