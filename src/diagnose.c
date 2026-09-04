#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "diagnose.h"
#include "text.h"
#include "win32_compat.h"

#define SYSTEM_EXTENDED_HANDLE_INFORMATION 64
#define STATUS_INFO_LENGTH_MISMATCH ((LONG)0xc0000004L)

typedef LONG (NTAPI *NtQuerySystemInformationFn)(ULONG, PVOID, ULONG, PULONG);

typedef struct {
    PVOID object;
    ULONG_PTR unique_process_id;
    ULONG_PTR handle_value;
    ULONG granted_access;
    USHORT creator_backtrace_index;
    USHORT object_type_index;
    ULONG handle_attributes;
    ULONG reserved;
} NativeHandleEntry;

typedef struct {
    ULONG_PTR number_of_handles;
    ULONG_PTR reserved;
    NativeHandleEntry handles[1];
} NativeHandleSnapshot;

typedef struct {
    DWORD pid;
    HANDLE handle;
    wchar_t *image;
    wchar_t *user;
    ULONGLONG creation_time;
    DWORD session_id;
    DWORD open_error;
    int access_issue_counted;
} ProcessCacheEntry;

typedef struct {
    ProcessCacheEntry *items;
    size_t count;
    size_t capacity;
} ProcessCache;

USB_EJECT_STATIC_ASSERT(native_handle_entry_x64, sizeof(NativeHandleEntry) == 40);

static wchar_t *wide_duplicate(const wchar_t *value) {
    size_t length;
    wchar_t *copy;
    if (value == NULL) value = L"";
    length = wcslen(value);
    if (length > ((size_t)-1) / sizeof(wchar_t) - 1) return NULL;
    copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (copy != NULL) memcpy(copy, value, (length + 1) * sizeof(wchar_t));
    return copy;
}

DiagnosticCompleteness diagnostic_completeness_from_issues(unsigned issues) {
    if (issues & DIAG_ISSUE_CHANGED) return DIAGNOSTIC_CHANGED_DURING_SCAN;
    if (issues & DIAG_ISSUE_TIMEOUT) return DIAGNOSTIC_PARTIAL_TIMEOUT;
    if (issues & DIAG_ISSUE_ACCESS) return DIAGNOSTIC_PARTIAL_ACCESS;
    if (issues & DIAG_ISSUE_KERNEL) return DIAGNOSTIC_KERNEL_OR_DRIVER;
    if (issues & DIAG_ISSUE_UNRESOLVED) return DIAGNOSTIC_UNRESOLVED;
    return DIAGNOSTIC_COMPLETE;
}

const wchar_t *diagnostic_completeness_name(DiagnosticCompleteness value) {
    switch (value) {
        case DIAGNOSTIC_COMPLETE: return L"complete";
        case DIAGNOSTIC_PARTIAL_ACCESS: return L"partial-access";
        case DIAGNOSTIC_PARTIAL_TIMEOUT: return L"partial-timeout";
        case DIAGNOSTIC_KERNEL_OR_DRIVER: return L"kernel-or-driver";
        case DIAGNOSTIC_CHANGED_DURING_SCAN: return L"changed-during-scan";
        default: return L"unresolved";
    }
}

void diagnostic_report_init(DiagnosticReport *report) {
    memset(report, 0, sizeof(*report));
    report->completeness = DIAGNOSTIC_UNRESOLVED;
}

void diagnostic_report_dispose(DiagnosticReport *report) {
    size_t index;
    if (report == NULL) return;
    for (index = 0; index < report->finding_count; index++) {
        free(report->findings[index].process_image);
        free(report->findings[index].process_user);
        {
            size_t service_index;
            for (service_index = 0;
                 service_index < report->findings[index].service_count;
                 service_index++) {
                free(report->findings[index].services[service_index]);
            }
            free(report->findings[index].services);
        }
        free(report->findings[index].dos_path);
        free(report->findings[index].nt_path);
    }
    free(report->findings);
    diagnostic_report_init(report);
}

static int enable_debug_privilege(void) {
    HANDLE token;
    TOKEN_PRIVILEGES privileges;
    LUID luid;
    BOOL adjusted;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return 0;
    }
    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return 0;
    }
    memset(&privileges, 0, sizeof(privileges));
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    adjusted = AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
    CloseHandle(token);
    return adjusted && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

static void *capture_snapshot(size_t *size_out, AppError *error) {
    HMODULE ntdll;
    NtQuerySystemInformationFn query;
    unsigned char *buffer;
    ULONG capacity;
    ULONG returned;
    LONG status;

    ntdll = GetModuleHandleW(L"ntdll.dll");
    query = ntdll != NULL
        ? (NtQuerySystemInformationFn)GetProcAddress(ntdll, "NtQuerySystemInformation")
        : NULL;
    if (query == NULL) {
        if (error != NULL) error->operation = L"resolve NtQuerySystemInformation";
        return NULL;
    }

    capacity = 1024U * 1024U;
    buffer = NULL;
    for (;;) {
        unsigned char *new_buffer;
        new_buffer = (unsigned char *)realloc(buffer, capacity);
        if (new_buffer == NULL) {
            free(buffer);
            if (error != NULL) {
                error->status = APP_OUT_OF_MEMORY;
                error->win32_error = ERROR_NOT_ENOUGH_MEMORY;
                error->operation = L"allocate handle snapshot";
            }
            return NULL;
        }
        buffer = new_buffer;
        returned = 0;
        status = query(SYSTEM_EXTENDED_HANDLE_INFORMATION,
            buffer, capacity, &returned);
        if (status >= 0) {
            *size_out = returned != 0 && returned <= capacity ? returned : capacity;
            return buffer;
        }
        if (status != STATUS_INFO_LENGTH_MISMATCH || capacity >= 256U * 1024U * 1024U) {
            free(buffer);
            if (error != NULL) {
                error->status = APP_DIAGNOSTIC_INCOMPLETE;
                error->nt_status = status;
                error->operation = L"query system handle snapshot";
            }
            return NULL;
        }
        if (returned > capacity && returned <= 256U * 1024U * 1024U) capacity = returned;
        else capacity *= 2;
    }
}

static int snapshot_valid(const NativeHandleSnapshot *snapshot, size_t bytes) {
    size_t available;
    if (bytes < offsetof(NativeHandleSnapshot, handles)) return 0;
    available = (bytes - offsetof(NativeHandleSnapshot, handles)) /
        sizeof(NativeHandleEntry);
    return snapshot->number_of_handles <= available;
}

static USHORT calibrate_file_type(
    const NativeHandleSnapshot *snapshot,
    HANDLE calibration_handle)
{
    ULONG_PTR index;
    ULONG_PTR handle_value;
    ULONG_PTR pid;
    handle_value = (ULONG_PTR)calibration_handle;
    pid = (ULONG_PTR)GetCurrentProcessId();
    for (index = 0; index < snapshot->number_of_handles; index++) {
        if (snapshot->handles[index].unique_process_id == pid &&
            snapshot->handles[index].handle_value == handle_value) {
            return snapshot->handles[index].object_type_index;
        }
    }
    return 0;
}

static ULONGLONG filetime_value(const FILETIME *value) {
    return ((ULONGLONG)value->dwHighDateTime << 32) | value->dwLowDateTime;
}

static wchar_t *query_process_image(HANDLE process) {
    DWORD capacity;
    DWORD length;
    wchar_t *buffer;
    capacity = 512;
    while (capacity <= 32768) {
        buffer = (wchar_t *)calloc(capacity, sizeof(wchar_t));
        if (buffer == NULL) return NULL;
        length = capacity;
        if (QueryFullProcessImageNameW(process, 0, buffer, &length)) return buffer;
        free(buffer);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return NULL;
        capacity *= 2;
    }
    return NULL;
}

static wchar_t *query_process_user(HANDLE process) {
    HANDLE token;
    DWORD needed;
    unsigned char *token_buffer;
    PTOKEN_USER token_user;
    DWORD name_length;
    DWORD domain_length;
    wchar_t *name;
    wchar_t *domain;
    SID_NAME_USE sid_type;
    wchar_t *combined;
    size_t total;

    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return NULL;
    needed = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &needed);
    if (needed == 0 || needed > 1024U * 1024U) {
        CloseHandle(token);
        return NULL;
    }
    token_buffer = (unsigned char *)malloc(needed);
    if (token_buffer == NULL) {
        CloseHandle(token);
        return NULL;
    }
    if (!GetTokenInformation(token, TokenUser, token_buffer, needed, &needed)) {
        free(token_buffer);
        CloseHandle(token);
        return NULL;
    }
    token_user = (PTOKEN_USER)token_buffer;
    name_length = 0;
    domain_length = 0;
    LookupAccountSidW(NULL, token_user->User.Sid, NULL, &name_length,
        NULL, &domain_length, &sid_type);
    if (name_length == 0 || name_length > 32768 || domain_length > 32768) {
        free(token_buffer);
        CloseHandle(token);
        return NULL;
    }
    name = (wchar_t *)calloc(name_length, sizeof(wchar_t));
    domain = (wchar_t *)calloc(domain_length == 0 ? 1 : domain_length, sizeof(wchar_t));
    if (name == NULL || domain == NULL || !LookupAccountSidW(NULL,
            token_user->User.Sid, name, &name_length, domain, &domain_length, &sid_type)) {
        free(name);
        free(domain);
        free(token_buffer);
        CloseHandle(token);
        return NULL;
    }
    total = wcslen(name) + wcslen(domain) + 2;
    combined = (wchar_t *)calloc(total, sizeof(wchar_t));
    if (combined != NULL) {
        if (domain[0] != L'\0') {
            wcscpy(combined, domain);
            wcscat(combined, L"\\");
        }
        wcscat(combined, name);
    }
    free(name);
    free(domain);
    free(token_buffer);
    CloseHandle(token);
    return combined;
}

static ProcessCacheEntry *process_cache_get(ProcessCache *cache, DWORD pid) {
    size_t index;
    ProcessCacheEntry *new_items;
    size_t new_capacity;
    ProcessCacheEntry *entry;
    FILETIME creation;
    FILETIME exit_time;
    FILETIME kernel;
    FILETIME user;

    for (index = 0; index < cache->count; index++) {
        if (cache->items[index].pid == pid) return &cache->items[index];
    }
    if (cache->count == cache->capacity) {
        new_capacity = cache->capacity == 0 ? 16 : cache->capacity * 2;
        if (new_capacity < cache->capacity ||
            new_capacity > ((size_t)-1) / sizeof(ProcessCacheEntry)) return NULL;
        new_items = (ProcessCacheEntry *)realloc(
            cache->items, new_capacity * sizeof(ProcessCacheEntry));
        if (new_items == NULL) return NULL;
        cache->items = new_items;
        cache->capacity = new_capacity;
    }
    entry = &cache->items[cache->count++];
    memset(entry, 0, sizeof(*entry));
    entry->pid = pid;
    entry->handle = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (entry->handle == NULL) {
        entry->open_error = GetLastError();
        return entry;
    }
    entry->image = query_process_image(entry->handle);
    entry->user = query_process_user(entry->handle);
    ProcessIdToSessionId(pid, &entry->session_id);
    if (GetProcessTimes(entry->handle, &creation, &exit_time, &kernel, &user)) {
        entry->creation_time = filetime_value(&creation);
    }
    return entry;
}

static void process_cache_dispose(ProcessCache *cache) {
    size_t index;
    for (index = 0; index < cache->count; index++) {
        if (cache->items[index].handle != NULL) CloseHandle(cache->items[index].handle);
        free(cache->items[index].image);
        free(cache->items[index].user);
    }
    free(cache->items);
    memset(cache, 0, sizeof(*cache));
}

static wchar_t *query_nt_file_path(HANDLE handle, DWORD *error_code) {
    DWORD capacity;
    DWORD length;
    wchar_t *buffer;

    capacity = 512;
    while (capacity <= 32768) {
        buffer = (wchar_t *)calloc(capacity, sizeof(wchar_t));
        if (buffer == NULL) {
            *error_code = ERROR_NOT_ENOUGH_MEMORY;
            return NULL;
        }
        length = GetFinalPathNameByHandleW(handle, buffer, capacity,
            VOLUME_NAME_NT | FILE_NAME_OPENED);
        if (length > 0 && length < capacity) return buffer;
        free(buffer);
        if (length == 0) {
            *error_code = GetLastError();
            return NULL;
        }
        capacity = length + 1;
    }
    *error_code = ERROR_INSUFFICIENT_BUFFER;
    return NULL;
}

static int path_matches_target(
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    const wchar_t *path)
{
    size_t index;
    const VolumeInfo *selected;
    selected = &inventory->volumes[target->volume_index];
    for (index = 0; index < inventory->count; index++) {
        if (inventory->volumes[index].removal_devinst == selected->removal_devinst &&
            inventory->volumes[index].native_path != NULL &&
            text_path_is_at_or_below(path, inventory->volumes[index].native_path)) {
            return 1;
        }
    }
    return 0;
}

static int append_finding(
    DiagnosticReport *report,
    const NativeHandleEntry *native,
    const ProcessCacheEntry *process,
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    const wchar_t *path)
{
    BlockerFinding *new_items;
    size_t new_capacity;
    BlockerFinding *finding;
    const VolumeInfo *selected;
    const VolumeInfo *volume;
    size_t volume_index;
    size_t root_length;
    const wchar_t *suffix;
    const wchar_t *mount;
    size_t mount_length;
    size_t suffix_length;

    if (report->finding_count == report->finding_capacity) {
        new_capacity = report->finding_capacity == 0 ? 8 : report->finding_capacity * 2;
        if (new_capacity < report->finding_capacity ||
            new_capacity > ((size_t)-1) / sizeof(BlockerFinding)) return 0;
        new_items = (BlockerFinding *)realloc(
            report->findings, new_capacity * sizeof(BlockerFinding));
        if (new_items == NULL) return 0;
        report->findings = new_items;
        report->finding_capacity = new_capacity;
    }
    finding = &report->findings[report->finding_count];
    memset(finding, 0, sizeof(*finding));
    finding->pid = (DWORD)native->unique_process_id;
    finding->handle_value = native->handle_value;
    finding->granted_access = native->granted_access;
    finding->session_id = process->session_id;
    finding->process_creation_time = process->creation_time;
    finding->process_image = wide_duplicate(process->image);
    finding->process_user = wide_duplicate(process->user);
    finding->nt_path = wide_duplicate(path);
    selected = &inventory->volumes[target->volume_index];
    for (volume_index = 0; volume_index < inventory->count; volume_index++) {
        volume = &inventory->volumes[volume_index];
        if (volume->removal_devinst != selected->removal_devinst ||
            volume->native_path == NULL || volume->mount_count == 0 ||
            !text_path_is_at_or_below(path, volume->native_path)) continue;
        root_length = wcslen(volume->native_path);
        suffix = path + root_length;
        while (*suffix == L'\\' || *suffix == L'/') suffix++;
        mount = volume->mount_points[0];
        mount_length = wcslen(mount);
        suffix_length = wcslen(suffix);
        if (mount_length <= (size_t)-1 - suffix_length - 2) {
            finding->dos_path = (wchar_t *)calloc(
                mount_length + suffix_length + 2, sizeof(wchar_t));
        }
        if (finding->dos_path != NULL) {
            wcscpy(finding->dos_path, mount);
            if (mount_length != 0 && mount[mount_length - 1] != L'\\' &&
                suffix_length != 0) wcscat(finding->dos_path, L"\\");
            wcscat(finding->dos_path, suffix);
        }
        break;
    }
    if (finding->process_image == NULL || finding->process_user == NULL ||
        finding->nt_path == NULL ||
        finding->dos_path == NULL) {
        free(finding->process_image);
        free(finding->process_user);
        free(finding->dos_path);
        free(finding->nt_path);
        memset(finding, 0, sizeof(*finding));
        return 0;
    }
    report->finding_count++;
    return 1;
}

static int finding_add_service(BlockerFinding *finding, const wchar_t *name) {
    wchar_t **new_values;
    wchar_t *copy;
    size_t new_count;
    size_t index;

    for (index = 0; index < finding->service_count; index++) {
        if (text_iequals(finding->services[index], name)) return 1;
    }
    if (finding->service_count == (size_t)-1) return 0;
    new_count = finding->service_count + 1;
    if (new_count > ((size_t)-1) / sizeof(wchar_t *)) return 0;
    copy = wide_duplicate(name);
    if (copy == NULL) return 0;
    new_values = (wchar_t **)realloc(finding->services,
        new_count * sizeof(wchar_t *));
    if (new_values == NULL) {
        free(copy);
        return 0;
    }
    finding->services = new_values;
    finding->services[finding->service_count++] = copy;
    return 1;
}

static void attach_service_names(DiagnosticReport *report) {
    SC_HANDLE manager;
    DWORD needed;
    DWORD returned;
    DWORD resume;
    unsigned char *buffer;
    ENUM_SERVICE_STATUS_PROCESSW *services;
    DWORD service_index;
    size_t finding_index;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (manager == NULL) {
        report->issue_flags |= DIAG_ISSUE_ACCESS;
        return;
    }
    needed = 0;
    returned = 0;
    resume = 0;
    EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
        SERVICE_ACTIVE, NULL, 0, &needed, &returned, &resume, NULL);
    if (needed == 0 || needed > 1024U * 1024U) {
        CloseServiceHandle(manager);
        return;
    }
    buffer = (unsigned char *)malloc(needed);
    if (buffer == NULL) {
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        CloseServiceHandle(manager);
        return;
    }
    resume = 0;
    if (!EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
            SERVICE_ACTIVE, buffer, needed, &needed, &returned, &resume, NULL)) {
        if (GetLastError() == ERROR_ACCESS_DENIED) report->issue_flags |= DIAG_ISSUE_ACCESS;
        free(buffer);
        CloseServiceHandle(manager);
        return;
    }
    services = (ENUM_SERVICE_STATUS_PROCESSW *)buffer;
    for (service_index = 0; service_index < returned; service_index++) {
        if (services[service_index].ServiceStatusProcess.dwProcessId == 0) continue;
        for (finding_index = 0; finding_index < report->finding_count; finding_index++) {
            if (report->findings[finding_index].pid ==
                services[service_index].ServiceStatusProcess.dwProcessId) {
                if (!finding_add_service(&report->findings[finding_index],
                        services[service_index].lpServiceName)) {
                    report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
                }
            }
        }
    }
    free(buffer);
    CloseServiceHandle(manager);
}

AppStatus diagnostic_scan(
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    DiagnosticReport *report,
    AppError *error)
{
    wchar_t module[32768];
    HANDLE calibration;
    void *buffer;
    size_t bytes;
    NativeHandleSnapshot *snapshot;
    USHORT file_type;
    ULONG_PTR index;
    NativeHandleEntry *native;
    ProcessCache cache;
    ProcessCacheEntry *process;
    HANDLE duplicate;
    wchar_t *path;
    DWORD path_error;
    DWORD pid;
    ULONG_PTR calibration_value;

    app_error_clear(error);
    diagnostic_report_init(report);
    memset(&cache, 0, sizeof(cache));
    report->debug_privilege_enabled = enable_debug_privilege();

    if (GetModuleFileNameW(NULL, module, 32768) == 0) {
        if (error != NULL) {
            error->status = APP_DIAGNOSTIC_INCOMPLETE;
            error->win32_error = GetLastError();
            error->operation = L"locate diagnostic executable";
        }
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
        return APP_DIAGNOSTIC_INCOMPLETE;
    }
    calibration = CreateFileW(module, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (calibration == INVALID_HANDLE_VALUE) {
        if (error != NULL) {
            error->status = APP_DIAGNOSTIC_INCOMPLETE;
            error->win32_error = GetLastError();
            error->operation = L"open calibration file";
        }
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
        return APP_DIAGNOSTIC_INCOMPLETE;
    }

    buffer = capture_snapshot(&bytes, error);
    if (buffer == NULL) {
        CloseHandle(calibration);
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
        return APP_DIAGNOSTIC_INCOMPLETE;
    }
    snapshot = (NativeHandleSnapshot *)buffer;
    if (!snapshot_valid(snapshot, bytes)) {
        free(buffer);
        CloseHandle(calibration);
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
        if (error != NULL) {
            error->status = APP_DIAGNOSTIC_INCOMPLETE;
            error->operation = L"validate native handle snapshot";
        }
        return APP_DIAGNOSTIC_INCOMPLETE;
    }
    file_type = calibrate_file_type(snapshot, calibration);
    if (file_type == 0) {
        free(buffer);
        CloseHandle(calibration);
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
        if (error != NULL) {
            error->status = APP_DIAGNOSTIC_INCOMPLETE;
            error->operation = L"calibrate File object type";
        }
        return APP_DIAGNOSTIC_INCOMPLETE;
    }
    calibration_value = (ULONG_PTR)calibration;

    for (index = 0; index < snapshot->number_of_handles; index++) {
        native = &snapshot->handles[index];
        if (native->object_type_index != file_type ||
            native->unique_process_id > 0xffffffffU) continue;
        pid = (DWORD)native->unique_process_id;
        if (pid == GetCurrentProcessId() &&
            native->handle_value == calibration_value) continue;
        process = process_cache_get(&cache, pid);
        if (process == NULL) {
            report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
            break;
        }
        if (process->handle == NULL) {
            if (process->open_error != ERROR_INVALID_PARAMETER &&
                !process->access_issue_counted) {
                report->issue_flags |= DIAG_ISSUE_ACCESS;
                report->inaccessible_process_count++;
                process->access_issue_counted = 1;
            }
            continue;
        }
        duplicate = NULL;
        if (!DuplicateHandle(process->handle, (HANDLE)native->handle_value,
                GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            report->issue_flags |= DIAG_ISSUE_CHANGED;
            report->changed_handle_count++;
            continue;
        }
        if (GetFileType(duplicate) != FILE_TYPE_DISK) {
            CloseHandle(duplicate);
            continue;
        }
        report->inspected_file_handle_count++;
        path_error = ERROR_SUCCESS;
        path = query_nt_file_path(duplicate, &path_error);
        CloseHandle(duplicate);
        if (path == NULL) {
            if (path_error == ERROR_INVALID_HANDLE) {
                report->issue_flags |= DIAG_ISSUE_CHANGED;
                report->changed_handle_count++;
            } else if (path_error == ERROR_ACCESS_DENIED) {
                report->issue_flags |= DIAG_ISSUE_ACCESS;
            }
            continue;
        }
        if (path_matches_target(inventory, target, path) &&
            !append_finding(report, native, process, inventory, target, path)) {
            free(path);
            report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
            break;
        }
        free(path);
    }

    attach_service_names(report);
    process_cache_dispose(&cache);
    free(buffer);
    CloseHandle(calibration);
    report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
    return report->completeness == DIAGNOSTIC_COMPLETE
        ? APP_OK : APP_DIAGNOSTIC_INCOMPLETE;
}
