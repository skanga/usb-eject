#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "target.h"
#include "text.h"

static int is_alpha_ascii(wchar_t value) {
    return (value >= L'A' && value <= L'Z') ||
           (value >= L'a' && value <= L'z');
}

static wchar_t upper_ascii(wchar_t value) {
    if (value >= L'a' && value <= L'z') return value - (L'a' - L'A');
    return value;
}

static int parse_drive_letter(const wchar_t *value, wchar_t *letter) {
    size_t length;
    if (value == NULL) return 0;
    length = wcslen(value);
    if ((length != 1 && length != 2) || !is_alpha_ascii(value[0])) return 0;
    if (length == 2 && value[1] != L':') return 0;
    *letter = upper_ascii(value[0]);
    return 1;
}

static int mount_has_letter(const wchar_t *mount, wchar_t letter) {
    return mount != NULL && upper_ascii(mount[0]) == letter &&
        mount[1] == L':' && (mount[2] == L'\\' || mount[2] == L'\0');
}

static wchar_t *wide_duplicate(const wchar_t *value) {
    size_t length;
    wchar_t *copy;
    length = wcslen(value);
    if (length > ((size_t)-1) / sizeof(wchar_t) - 1) return NULL;
    copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (copy != NULL) memcpy(copy, value, (length + 1) * sizeof(wchar_t));
    return copy;
}

static wchar_t *full_path(const wchar_t *value, DWORD *error_code) {
    DWORD needed;
    wchar_t *buffer;
    DWORD result;

    needed = GetFullPathNameW(value, 0, NULL, NULL);
    if (needed == 0 || needed > 32768) {
        *error_code = GetLastError();
        return NULL;
    }
    buffer = (wchar_t *)calloc((size_t)needed + 1, sizeof(wchar_t));
    if (buffer == NULL) {
        *error_code = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    result = GetFullPathNameW(value, needed + 1, buffer, NULL);
    if (result == 0 || result > needed) {
        *error_code = GetLastError();
        free(buffer);
        return NULL;
    }
    return buffer;
}

static wchar_t *module_path(DWORD *error_code) {
    DWORD capacity;
    wchar_t *buffer;
    DWORD length;

    capacity = 512;
    while (capacity <= 32768) {
        buffer = (wchar_t *)calloc(capacity, sizeof(wchar_t));
        if (buffer == NULL) {
            *error_code = ERROR_NOT_ENOUGH_MEMORY;
            return NULL;
        }
        SetLastError(ERROR_SUCCESS);
        length = GetModuleFileNameW(NULL, buffer, capacity);
        if (length > 0 && length < capacity - 1) return buffer;
        free(buffer);
        if (length == 0) {
            *error_code = GetLastError();
            return NULL;
        }
        capacity *= 2;
    }
    *error_code = ERROR_INSUFFICIENT_BUFFER;
    return NULL;
}

static wchar_t *path_volume_mount(const wchar_t *path, DWORD *error_code) {
    wchar_t *absolute;
    wchar_t *mount;

    absolute = full_path(path, error_code);
    if (absolute == NULL) return NULL;
    mount = (wchar_t *)calloc(32768, sizeof(wchar_t));
    if (mount == NULL) {
        free(absolute);
        *error_code = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (!GetVolumePathNameW(absolute, mount, 32768)) {
        *error_code = GetLastError();
        free(absolute);
        free(mount);
        return NULL;
    }
    free(absolute);
    return mount;
}

static int mount_matches(const wchar_t *left, const wchar_t *right) {
    size_t left_length;
    size_t right_length;
    wchar_t *left_copy;
    wchar_t *right_copy;
    int result;

    if (left == NULL || right == NULL) return 0;
    left_copy = wide_duplicate(left);
    right_copy = wide_duplicate(right);
    if (left_copy == NULL || right_copy == NULL) {
        free(left_copy);
        free(right_copy);
        return 0;
    }
    left_length = wcslen(left_copy);
    right_length = wcslen(right_copy);
    while (left_length > 3 &&
        (left_copy[left_length - 1] == L'\\' || left_copy[left_length - 1] == L'/')) {
        left_copy[--left_length] = L'\0';
    }
    while (right_length > 3 &&
        (right_copy[right_length - 1] == L'\\' || right_copy[right_length - 1] == L'/')) {
        right_copy[--right_length] = L'\0';
    }
    result = text_iequals(left_copy, right_copy);
    free(left_copy);
    free(right_copy);
    return result;
}

static int match_mount(const VolumeInfo *volume, const wchar_t *mount) {
    size_t index;
    for (index = 0; index < volume->mount_count; index++) {
        if (mount_matches(volume->mount_points[index], mount)) return 1;
    }
    return 0;
}

static int match_letter(const VolumeInfo *volume, wchar_t letter) {
    size_t index;
    for (index = 0; index < volume->mount_count; index++) {
        if (mount_has_letter(volume->mount_points[index], letter)) return 1;
    }
    return 0;
}

static int match_name(const VolumeInfo *volume, const wchar_t *pattern) {
    size_t vendor_length;
    size_t product_length;
    size_t revision_length;
    size_t total;
    wchar_t *combined;
    int result;

    if (text_match_pattern(volume->vendor, pattern) ||
        text_match_pattern(volume->product, pattern) ||
        text_match_pattern(volume->revision, pattern) ||
        text_match_pattern(volume->instance_id, pattern)) return 1;

    vendor_length = volume->vendor != NULL ? wcslen(volume->vendor) : 0;
    product_length = volume->product != NULL ? wcslen(volume->product) : 0;
    revision_length = volume->revision != NULL ? wcslen(volume->revision) : 0;
    total = vendor_length + product_length + revision_length + 3;
    if (total < vendor_length || total > 32768) return 0;
    combined = (wchar_t *)calloc(total, sizeof(wchar_t));
    if (combined == NULL) return 0;
    if (vendor_length) wcscat(combined, volume->vendor);
    if (vendor_length && product_length) wcscat(combined, L" ");
    if (product_length) wcscat(combined, volume->product);
    if ((vendor_length || product_length) && revision_length) wcscat(combined, L" ");
    if (revision_length) wcscat(combined, volume->revision);
    result = text_match_pattern(combined, pattern);
    free(combined);
    return result;
}

static int same_device(const VolumeInfo *left, const VolumeInfo *right) {
    if (left->removal_devinst != 0 && right->removal_devinst != 0) {
        return left->removal_devinst == right->removal_devinst;
    }
    return left == right;
}

int target_same_media(const VolumeInfo *left, const VolumeInfo *right) {
    return left->disk_devinst != 0 && left->disk_devinst == right->disk_devinst &&
        left->device_type == right->device_type && left->device_number == right->device_number;
}

int target_volume_in_scope(const DeviceInventory *inventory,
    const ResolvedTarget *target, size_t index) {
    const VolumeInfo *selected = &inventory->volumes[target->volume_index];
    if (index == target->volume_index) return 1;
    return target->media_scope ? target_same_media(selected, &inventory->volumes[index]) :
        same_device(selected, &inventory->volumes[index]);
}

void resolved_target_dispose(ResolvedTarget *resolved) {
    if (resolved == NULL) return;
    free(resolved->distinct_volume_indexes);
    memset(resolved, 0, sizeof(*resolved));
}

AppStatus target_resolve_mode(
    const DeviceInventory *inventory,
    const TargetSelector *selector,
    int media_scope,
    ResolvedTarget *resolved,
    AppError *error)
{
    unsigned char *matched;
    wchar_t letter;
    wchar_t *resolved_mount;
    wchar_t *self_path;
    DWORD code;
    size_t index;
    size_t previous;
    int match;
    AppStatus status;
    size_t *distinct_indexes;

    memset(resolved, 0, sizeof(*resolved));
    resolved->media_scope = media_scope;
    app_error_clear(error);
    if (inventory == NULL || selector == NULL) return APP_INTERNAL_ERROR;
    matched = (unsigned char *)calloc(inventory->count == 0 ? 1 : inventory->count, 1);
    if (matched == NULL) {
        if (error != NULL) {
            error->status = APP_OUT_OF_MEMORY;
            error->win32_error = ERROR_NOT_ENOUGH_MEMORY;
            error->operation = L"allocate target matches";
        }
        return APP_OUT_OF_MEMORY;
    }
    resolved_mount = NULL;
    self_path = NULL;
    code = ERROR_SUCCESS;
    distinct_indexes = NULL;

    if (selector->kind == SELECTOR_IMPLICIT &&
        parse_drive_letter(selector->value, &letter)) {
        /* handled as a letter below */
    } else if (selector->kind == SELECTOR_IMPLICIT || selector->kind == SELECTOR_MOUNT ||
               selector->kind == SELECTOR_THIS) {
        if (selector->kind == SELECTOR_THIS) {
            self_path = module_path(&code);
            if (self_path != NULL) resolved_mount = path_volume_mount(self_path, &code);
        } else {
            resolved_mount = path_volume_mount(selector->value, &code);
        }
        if (resolved_mount == NULL) {
            free(self_path);
            free(matched);
            if (error != NULL) {
                error->status = code == ERROR_ACCESS_DENIED ? APP_ACCESS_DENIED : APP_NOT_FOUND;
                error->win32_error = code;
                error->operation = L"resolve target path";
            }
            return code == ERROR_ACCESS_DENIED ? APP_ACCESS_DENIED : APP_NOT_FOUND;
        }
    } else if (selector->kind == SELECTOR_LETTER &&
               !parse_drive_letter(selector->value, &letter)) {
        free(matched);
        if (error != NULL) {
            error->status = APP_USAGE;
            error->operation = L"validate drive letter";
        }
        return APP_USAGE;
    }

    for (index = 0; index < inventory->count; index++) {
        match = 0;
        if (selector->kind == SELECTOR_LETTER ||
            (selector->kind == SELECTOR_IMPLICIT && resolved_mount == NULL)) {
            match = match_letter(&inventory->volumes[index], letter);
        } else if (resolved_mount != NULL) {
            match = match_mount(&inventory->volumes[index], resolved_mount);
        } else if (selector->kind == SELECTOR_LABEL && inventory->volumes[index].mount_count != 0) {
            match = text_match_pattern(inventory->volumes[index].label, selector->value);
        } else if (selector->kind == SELECTOR_NAME && inventory->volumes[index].mount_count != 0) {
            match = match_name(&inventory->volumes[index], selector->value);
        }
        if (match) matched[index] = 1;
    }
    free(resolved_mount);
    free(self_path);

    if (inventory->count != 0) {
        distinct_indexes = (size_t *)malloc(inventory->count * sizeof(size_t));
        if (distinct_indexes == NULL) {
            free(matched);
            if (error != NULL) {
                error->status = APP_OUT_OF_MEMORY;
                error->win32_error = ERROR_NOT_ENOUGH_MEMORY;
                error->operation = L"allocate ambiguous target matches";
            }
            return APP_OUT_OF_MEMORY;
        }
    }

    for (index = 0; index < inventory->count; index++) {
        if (!matched[index]) continue;
        for (previous = 0; previous < index; previous++) {
            if (matched[previous] && (media_scope ? target_same_media(
                    &inventory->volumes[previous], &inventory->volumes[index]) : same_device(
                    &inventory->volumes[previous], &inventory->volumes[index]))) break;
        }
        if (previous == index) {
            if (resolved->distinct_match_count == 0) resolved->volume_index = index;
            distinct_indexes[resolved->distinct_match_count] = index;
            resolved->distinct_match_count++;
        }
    }
    free(matched);
    resolved->distinct_volume_indexes = distinct_indexes;

    if (resolved->distinct_match_count == 0) return APP_NOT_FOUND;
    if (resolved->distinct_match_count > 1) return APP_AMBIGUOUS;
    resolved->removal_devinst = inventory->volumes[resolved->volume_index].removal_devinst;
    for (index = 0; index < inventory->count; index++) {
        if (target_volume_in_scope(inventory, resolved, index)) {
            resolved->sibling_volume_count++;
        }
    }
    status = resolved->removal_devinst == 0 ? APP_UNSUPPORTED : APP_OK;
    return status;
}

AppStatus target_resolve(const DeviceInventory *inventory,
    const TargetSelector *selector, ResolvedTarget *resolved, AppError *error) {
    return target_resolve_mode(inventory, selector, 0, resolved, error);
}
