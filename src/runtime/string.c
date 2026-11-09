#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/string.h"

static int runtime_string_is_space(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

size_t runtime_string_length(const char *text)
{
    if (!text) {
        return 0;
    }
    return strlen(text);
}

int runtime_string_contains(const char *text, const char *needle)
{
    if (!text || !needle) {
        return 0;
    }

    return strstr(text, needle) != NULL;
}

int runtime_string_starts_with(const char *text, const char *prefix)
{
    size_t text_length;
    size_t prefix_length;

    if (!text || !prefix) {
        return 0;
    }

    text_length = strlen(text);
    prefix_length = strlen(prefix);

    if (prefix_length > text_length) {
        return 0;
    }

    return memcmp(text, prefix, prefix_length) == 0;
}

int runtime_string_ends_with(const char *text, const char *suffix)
{
    size_t text_length;
    size_t suffix_length;

    if (!text || !suffix) {
        return 0;
    }

    text_length = strlen(text);
    suffix_length = strlen(suffix);

    if (suffix_length > text_length) {
        return 0;
    }

    return memcmp(text + (text_length - suffix_length), suffix, suffix_length) == 0;
}

char *runtime_string_trim(const char *text)
{
    const char *start;
    const char *end;
    size_t length;
    char *result;

    if (!text) {
        return NULL;
    }

    start = text;
    while (*start != '\0' && runtime_string_is_space((unsigned char)*start)) {
        start++;
    }

    end = start + strlen(start);
    while (end > start && runtime_string_is_space((unsigned char)*(end - 1))) {
        end--;
    }

    length = (size_t)(end - start);
    result = malloc(length + 1);
    if (!result) {
        return NULL;
    }

    memcpy(result, start, length);
    result[length] = '\0';
    return result;
}

char *runtime_string_trim_start(const char *text)
{
    const char *start;
    size_t length;
    char *result;

    if (!text) {
        return NULL;
    }

    start = text;
    while (*start != '\0' && runtime_string_is_space((unsigned char)*start)) {
        start++;
    }

    length = strlen(start);
    result = malloc(length + 1);
    if (!result) {
        return NULL;
    }

    memcpy(result, start, length + 1);
    return result;
}

char *runtime_string_trim_end(const char *text)
{
    const char *end;
    size_t length;
    char *result;

    if (!text) {
        return NULL;
    }

    end = text + strlen(text);
    while (end > text && runtime_string_is_space((unsigned char)*(end - 1))) {
        end--;
    }

    length = (size_t)(end - text);
    result = malloc(length + 1);
    if (!result) {
        return NULL;
    }

    memcpy(result, text, length);
    result[length] = '\0';
    return result;
}

char *runtime_string_to_upper(const char *text)
{
    size_t length;
    char *result;
    size_t i;

    if (!text) {
        return NULL;
    }

    length = strlen(text);
    result = malloc(length + 1);
    if (!result) {
        return NULL;
    }

    for (i = 0; i < length; ++i) {
        result[i] = (char)toupper((unsigned char)text[i]);
    }
    result[length] = '\0';
    return result;
}

char *runtime_string_to_lower(const char *text)
{
    size_t length;
    char *result;
    size_t i;

    if (!text) {
        return NULL;
    }

    length = strlen(text);
    result = malloc(length + 1);
    if (!result) {
        return NULL;
    }

    for (i = 0; i < length; ++i) {
        result[i] = (char)tolower((unsigned char)text[i]);
    }
    result[length] = '\0';
    return result;
}
