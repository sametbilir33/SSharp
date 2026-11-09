#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/console.h"

void runtime_console_write(const char *text)
{
    if (!text) {
        return;
    }

    fputs(text, stdout);
    fflush(stdout);
}

void runtime_console_write_line(const char *text)
{
    runtime_console_write(text);
    fputc('\n', stdout);
    fflush(stdout);
}

void runtime_console_write_int(long long value)
{
    printf("%lld", value);
    fflush(stdout);
}

void runtime_console_write_bool(int value)
{
    runtime_console_write(value ? "true" : "false");
}

char *runtime_console_read_line(void)
{
    size_t capacity = 32;
    size_t length = 0;
    char *buffer = malloc(capacity);
    int ch;

    if (!buffer) {
        return NULL;
    }

    while ((ch = fgetc(stdin)) != EOF && ch != '\n' && ch != '\r') {
        if (length + 1 >= capacity) {
            size_t next_capacity = capacity * 2;
            char *resized = realloc(buffer, next_capacity);
            if (!resized) {
                free(buffer);
                return NULL;
            }
            buffer = resized;
            capacity = next_capacity;
        }

        buffer[length++] = (char)ch;
    }

    buffer[length] = '\0';

    if (length == 0 && ch == EOF) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

void runtime_console_clear(void)
{
    fputs("\033[H\033[2J\033[3J", stdout);
    fflush(stdout);
}

void runtime_console_reset_color(void)
{
    fputs("\033[0m", stdout);
    fflush(stdout);
}
