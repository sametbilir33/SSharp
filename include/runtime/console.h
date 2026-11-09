#ifndef SSHARP_RUNTIME_CONSOLE_H
#define SSHARP_RUNTIME_CONSOLE_H

#include <stddef.h>

void runtime_console_write(const char *text);
void runtime_console_write_line(const char *text);
void runtime_console_write_int(long long value);
void runtime_console_write_bool(int value);
char *runtime_console_read_line(void);
void runtime_console_clear(void);
void runtime_console_reset_color(void);

#endif
