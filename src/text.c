#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stddef.h>
#include <wchar.h>

#include "text.h"

#ifndef CSTR_EQUAL
#define CSTR_EQUAL 2
#endif

__declspec(dllimport) int WINAPI CompareStringOrdinal(
    LPCWCH string1,
    int count1,
    LPCWCH string2,
    int count2,
    BOOL ignore_case);

static int equal_counted(
    const wchar_t *left,
    size_t left_length,
    const wchar_t *right,
    size_t right_length)
{
    if (left_length != right_length) {
        return 0;
    }
    if (left_length > 0x7fffffffU) {
        return 0;
    }
    return CompareStringOrdinal(
        left,
        (int)left_length,
        right,
        (int)right_length,
        TRUE) == CSTR_EQUAL;
}

int text_iequals(const wchar_t *left, const wchar_t *right) {
    if (left == NULL || right == NULL) {
        return left == right;
    }
    return equal_counted(left, wcslen(left), right, wcslen(right));
}

int text_pattern_valid(const wchar_t *pattern) {
    size_t length;
    size_t index;
    size_t stars;

    if (pattern == NULL || pattern[0] == L'\0') {
        return 0;
    }

    length = wcslen(pattern);
    stars = 0;
    for (index = 0; index < length; index++) {
        if (pattern[index] == L'*') {
            stars++;
            if (index != 0 && index != length - 1) {
                return 0;
            }
        }
    }

    return stars < length;
}

int text_match_pattern(const wchar_t *value, const wchar_t *pattern) {
    size_t value_length;
    size_t pattern_length;
    size_t literal_start;
    size_t literal_length;
    int leading_star;
    int trailing_star;
    size_t index;

    if (value == NULL || !text_pattern_valid(pattern)) {
        return 0;
    }

    value_length = wcslen(value);
    pattern_length = wcslen(pattern);
    leading_star = pattern[0] == L'*';
    trailing_star = pattern[pattern_length - 1] == L'*';
    literal_start = leading_star ? 1 : 0;
    literal_length = pattern_length - literal_start - (trailing_star ? 1 : 0);

    if (!leading_star && !trailing_star) {
        return equal_counted(value, value_length, pattern, pattern_length);
    }
    if (value_length < literal_length) {
        return 0;
    }
    if (!leading_star) {
        return equal_counted(value, literal_length, pattern, literal_length);
    }
    if (!trailing_star) {
        return equal_counted(
            value + value_length - literal_length,
            literal_length,
            pattern + literal_start,
            literal_length);
    }

    for (index = 0; index + literal_length <= value_length; index++) {
        if (equal_counted(
                value + index,
                literal_length,
                pattern + literal_start,
                literal_length)) {
            return 1;
        }
    }
    return 0;
}

static int is_separator(wchar_t value) {
    return value == L'\\' || value == L'/';
}

int text_path_is_at_or_below(const wchar_t *path, const wchar_t *root) {
    size_t path_length;
    size_t root_length;

    if (path == NULL || root == NULL) {
        return 0;
    }

    path_length = wcslen(path);
    root_length = wcslen(root);
    while (root_length > 1 && is_separator(root[root_length - 1])) {
        if (root_length == 3 && root[1] == L':') {
            break;
        }
        root_length--;
    }

    if (path_length < root_length ||
        !equal_counted(path, root_length, root, root_length)) {
        return 0;
    }
    if (path_length == root_length) {
        return 1;
    }
    if (is_separator(root[root_length - 1])) {
        return 1;
    }
    return is_separator(path[root_length]);
}

static void append_char(
    wchar_t value,
    wchar_t *output,
    size_t capacity,
    size_t *written,
    size_t *required)
{
    if (capacity > 0 && *written + 1 < capacity) {
        output[*written] = value;
        (*written)++;
    }
    (*required)++;
}

size_t text_tsv_escape(const wchar_t *input, wchar_t *output, size_t capacity) {
    size_t index;
    size_t written;
    size_t required;
    wchar_t escape;

    written = 0;
    required = 0;
    if (input == NULL) {
        input = L"";
    }

    for (index = 0; input[index] != L'\0'; index++) {
        escape = L'\0';
        if (input[index] == L'\\') escape = L'\\';
        else if (input[index] == L'\t') escape = L't';
        else if (input[index] == L'\r') escape = L'r';
        else if (input[index] == L'\n') escape = L'n';

        if (escape != L'\0') {
            append_char(L'\\', output, capacity, &written, &required);
            append_char(escape, output, capacity, &written, &required);
        } else {
            append_char(input[index], output, capacity, &written, &required);
        }
    }

    if (capacity > 0) {
        output[written] = L'\0';
    }
    return required;
}
