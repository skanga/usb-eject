#ifndef USB_EJECT_TEXT_H
#define USB_EJECT_TEXT_H

#include <stddef.h>
#include <wchar.h>

int text_iequals(const wchar_t *left, const wchar_t *right);
int text_pattern_valid(const wchar_t *pattern);
int text_match_pattern(const wchar_t *value, const wchar_t *pattern);
int text_path_is_at_or_below(const wchar_t *path, const wchar_t *root);

/* Returns the required length excluding NUL. Output is NUL-terminated when
   capacity is nonzero, including when truncation is required. */
size_t text_tsv_escape(const wchar_t *input, wchar_t *output, size_t capacity);

#endif
