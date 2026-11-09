#ifndef SSHARP_RUNTIME_STRING_H
#define SSHARP_RUNTIME_STRING_H

#include <stddef.h>

size_t runtime_string_length(const char *text);
int runtime_string_contains(const char *text, const char *needle);
int runtime_string_starts_with(const char *text, const char *prefix);
int runtime_string_ends_with(const char *text, const char *suffix);
char *runtime_string_trim(const char *text);
char *runtime_string_trim_start(const char *text);
char *runtime_string_trim_end(const char *text);
char *runtime_string_to_upper(const char *text);
char *runtime_string_to_lower(const char *text);

#endif
