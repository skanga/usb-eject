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

const wchar_t *diagnostic_classification_name(DiagnosticClassification value) {
    switch (value) {
        case DIAGNOSTIC_BLOCKING_CANDIDATE: return L"blocking-candidate";
        case DIAGNOSTIC_PROCESS_ON_DEVICE: return L"process-on-device";
        case DIAGNOSTIC_CONFIRMED_VETO: return L"confirmed-veto";
        case DIAGNOSTIC_UNRESOLVED_FINDING: return L"unresolved";
        case DIAGNOSTIC_SUMMARY: return L"diagnostic-summary";
        default: return L"unresolved";
    }
}

static int finding_compare(const void *left_value, const void *right_value) {
    const BlockerFinding *left;
    const BlockerFinding *right;
    const wchar_t *left_path;
    const wchar_t *right_path;
    int compared;
    left = (const BlockerFinding *)left_value;
    right = (const BlockerFinding *)right_value;
    if (left->pid != right->pid) return left->pid < right->pid ? -1 : 1;
    if (left->process_creation_time != right->process_creation_time) {
        return left->process_creation_time < right->process_creation_time ? -1 : 1;
    }
    if (left->classification != right->classification) {
        return left->classification < right->classification ? -1 : 1;
    }
    left_path = left->dos_path != NULL ? left->dos_path : L"";
    right_path = right->dos_path != NULL ? right->dos_path : L"";
    compared = _wcsicmp(left_path, right_path);
    if (compared != 0) return compared;
    if (left->handle_value == right->handle_value) return 0;
    return left->handle_value < right->handle_value ? -1 : 1;
}

void diagnostic_sort_report(DiagnosticReport *report) {
    if (report == NULL || report->finding_count < 2) return;
    qsort(report->findings, report->finding_count,
        sizeof(BlockerFinding), finding_compare);
}

void diagnostic_report_init(DiagnosticReport *report) {
    memset(report, 0, sizeof(*report));
    report->completeness = DIAGNOSTIC_UNRESOLVED;
}

static void report_inspection_error(
    DiagnosticReport *report,
    DWORD win32_error,
    const wchar_t *operation)
{
    if (report->last_operation == NULL) {
        report->last_win32_error = win32_error;
        report->last_operation = operation;
    }
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

static wchar_t *query_nt_file_path_direct(HANDLE handle, DWORD *error_code) {
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

typedef struct {
    ULONG_PTR handle_value;
} ResolveRequest;

typedef struct {
    DWORD error_code;
    DWORD character_count;
} ResolveResponse;

typedef struct {
    HANDLE process;
    HANDLE input;
    HANDLE output;
} ResolveWorker;

static int read_exact(HANDLE handle, void *buffer, DWORD size) {
    DWORD offset;
    DWORD read;
    offset = 0;
    while (offset < size) {
        read = 0;
        if (!ReadFile(handle, (unsigned char *)buffer + offset,
                size - offset, &read, NULL) || read == 0) return 0;
        offset += read;
    }
    return 1;
}

static int write_exact(HANDLE handle, const void *buffer, DWORD size) {
    DWORD offset;
    DWORD written;
    offset = 0;
    while (offset < size) {
        written = 0;
        if (!WriteFile(handle, (const unsigned char *)buffer + offset,
                size - offset, &written, NULL) || written == 0) return 0;
        offset += written;
    }
    return 1;
}

static int run_resolve_worker(void) {
    ResolveRequest request;
    ResolveResponse response;
    wchar_t *path;
    DWORD error_code;
    while (read_exact(GetStdHandle(STD_INPUT_HANDLE), &request, sizeof(request))) {
        error_code = ERROR_SUCCESS;
        path = query_nt_file_path_direct((HANDLE)request.handle_value, &error_code);
        CloseHandle((HANDLE)request.handle_value);
        memset(&response, 0, sizeof(response));
        response.error_code = error_code;
        if (path != NULL) response.character_count = (DWORD)wcslen(path) + 1;
        if (!write_exact(GetStdHandle(STD_OUTPUT_HANDLE), &response, sizeof(response)) ||
            (path != NULL && !write_exact(GetStdHandle(STD_OUTPUT_HANDLE), path,
                response.character_count * sizeof(wchar_t)))) {
            free(path);
            return APP_INTERNAL_ERROR;
        }
        free(path);
    }
    return APP_OK;
}

static int parse_hex_argument(
    const wchar_t **cursor,
    ULONG_PTR *parsed_value)
{
    ULONG_PTR value;
    unsigned digit;
    int found;
    while (**cursor == L' ' || **cursor == L'\t') (*cursor)++;
    value = 0;
    found = 0;
    while (**cursor != L'\0' && **cursor != L' ' && **cursor != L'\t') {
        if (**cursor >= L'0' && **cursor <= L'9') digit = (unsigned)(**cursor - L'0');
        else if (**cursor >= L'a' && **cursor <= L'f') digit = (unsigned)(**cursor - L'a') + 10;
        else if (**cursor >= L'A' && **cursor <= L'F') digit = (unsigned)(**cursor - L'A') + 10;
        else return 0;
        if (value > ((ULONG_PTR)-1 - digit) / 16) return 0;
        value = value * 16 + digit;
        found = 1;
        (*cursor)++;
    }
    *parsed_value = value;
    return found;
}

int diagnostic_run_worker_if_requested(int *exit_code) {
    const wchar_t *command_line;
    const wchar_t *argument;
    size_t marker_length;
    ULONG_PTR expected_input;
    ULONG_PTR expected_output;
    command_line = GetCommandLineW();
    if (*command_line == L'"') {
        command_line++;
        while (*command_line != L'\0' && *command_line != L'"') command_line++;
        if (*command_line == L'"') command_line++;
    } else {
        while (*command_line != L'\0' && *command_line != L' ' &&
               *command_line != L'\t') command_line++;
    }
    while (*command_line == L' ' || *command_line == L'\t') command_line++;
    argument = L"--internal-handle-worker";
    marker_length = wcslen(argument);
    if (wcsncmp(command_line, argument, marker_length) != 0 ||
        (command_line[marker_length] != L'\0' &&
         command_line[marker_length] != L' ' &&
         command_line[marker_length] != L'\t')) return 0;
    command_line += marker_length;
    if (!parse_hex_argument(&command_line, &expected_input) ||
        !parse_hex_argument(&command_line, &expected_output)) {
        *exit_code = APP_USAGE;
        return 1;
    }
    while (*command_line == L' ' || *command_line == L'\t') command_line++;
    if (*command_line != L'\0' ||
        expected_input != (ULONG_PTR)GetStdHandle(STD_INPUT_HANDLE) ||
        expected_output != (ULONG_PTR)GetStdHandle(STD_OUTPUT_HANDLE) ||
        GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_PIPE ||
        GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_PIPE) {
        *exit_code = APP_USAGE;
        return 1;
    }
    *exit_code = run_resolve_worker();
    return 1;
}

static void resolve_worker_stop(ResolveWorker *worker) {
    if (worker->input != NULL) CloseHandle(worker->input);
    if (worker->process != NULL) {
        if (WaitForSingleObject(worker->process, 100) == WAIT_TIMEOUT) {
            TerminateProcess(worker->process, APP_DIAGNOSTIC_INCOMPLETE);
        }
        CloseHandle(worker->process);
    }
    if (worker->output != NULL) CloseHandle(worker->output);
    memset(worker, 0, sizeof(*worker));
}

static int resolve_worker_start(ResolveWorker *worker, DWORD *error_code) {
    SECURITY_ATTRIBUTES security;
    HANDLE child_input;
    HANDLE child_output;
    wchar_t module[32768];
    wchar_t command_line[32768];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    int length;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    child_input = NULL;
    child_output = NULL;
    if (!CreatePipe(&child_input, &worker->input, &security, 0) ||
        !CreatePipe(&worker->output, &child_output, &security, 0)) {
        *error_code = GetLastError();
        if (child_input != NULL) CloseHandle(child_input);
        if (child_output != NULL) CloseHandle(child_output);
        resolve_worker_stop(worker);
        return 0;
    }
    if (!SetHandleInformation(worker->input, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(worker->output, HANDLE_FLAG_INHERIT, 0)) {
        *error_code = GetLastError();
        CloseHandle(child_input);
        CloseHandle(child_output);
        resolve_worker_stop(worker);
        return 0;
    }
    if (GetModuleFileNameW(NULL, module, 32768) == 0) {
        *error_code = GetLastError();
        CloseHandle(child_input);
        CloseHandle(child_output);
        resolve_worker_stop(worker);
        return 0;
    }
    length = _snwprintf(command_line, 32768,
        L"\"%ls\" --internal-handle-worker %016llX %016llX", module,
        (unsigned long long)(ULONG_PTR)child_input,
        (unsigned long long)(ULONG_PTR)child_output);
    if (length < 0 || length >= 32768) {
        *error_code = ERROR_INSUFFICIENT_BUFFER;
        CloseHandle(child_input);
        CloseHandle(child_output);
        resolve_worker_stop(worker);
        return 0;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_input;
    startup.hStdOutput = child_output;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(module, command_line, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        *error_code = GetLastError();
        CloseHandle(child_input);
        CloseHandle(child_output);
        resolve_worker_stop(worker);
        return 0;
    }
    CloseHandle(child_input);
    CloseHandle(child_output);
    CloseHandle(process.hThread);
    worker->process = process.hProcess;
    return 1;
}

typedef struct {
    HANDLE pipe;
    ResolveResponse *response;
    int succeeded;
} ResponseReadContext;

static DWORD WINAPI read_response_thread(LPVOID parameter) {
    ResponseReadContext *context;
    context = (ResponseReadContext *)parameter;
    context->succeeded = read_exact(context->pipe, context->response,
        sizeof(*context->response));
    return 0;
}

static int read_response_with_timeout(
    ResolveWorker *worker,
    ResolveResponse *response,
    int *timed_out,
    DWORD *error_code)
{
    ResponseReadContext context;
    HANDLE thread;
    DWORD wait_result;
    memset(&context, 0, sizeof(context));
    context.pipe = worker->output;
    context.response = response;
    thread = CreateThread(NULL, 0, read_response_thread, &context, 0, NULL);
    if (thread == NULL) {
        *error_code = GetLastError();
        return 0;
    }
    wait_result = WaitForSingleObject(thread, 1000);
    if (wait_result == WAIT_TIMEOUT) {
        *timed_out = 1;
        *error_code = WAIT_TIMEOUT;
        resolve_worker_stop(worker);
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
        return 0;
    }
    CloseHandle(thread);
    if (wait_result != WAIT_OBJECT_0 || !context.succeeded) {
        *error_code = ERROR_INVALID_DATA;
        return 0;
    }
    return 1;
}

static wchar_t *query_nt_file_path(
    ResolveWorker *worker,
    HANDLE handle,
    DWORD *error_code,
    int *timed_out)
{
    ResolveRequest request;
    ResolveResponse response;
    HANDLE remote_handle;
    wchar_t *path;
    *timed_out = 0;
    if (worker->process == NULL && !resolve_worker_start(worker, error_code)) return NULL;
    remote_handle = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), handle, worker->process,
            &remote_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        *error_code = GetLastError();
        return NULL;
    }
    request.handle_value = (ULONG_PTR)remote_handle;
    if (!write_exact(worker->input, &request, sizeof(request))) {
        *error_code = GetLastError();
        resolve_worker_stop(worker);
        return NULL;
    }
    if (!read_response_with_timeout(worker, &response, timed_out, error_code)) {
        if (worker->process != NULL) resolve_worker_stop(worker);
        return NULL;
    }
    if (response.character_count > 32768) {
        *error_code = ERROR_INVALID_DATA;
        resolve_worker_stop(worker);
        return NULL;
    }
    if (response.character_count == 0) {
        *error_code = response.error_code;
        return NULL;
    }
    path = (wchar_t *)calloc(response.character_count, sizeof(wchar_t));
    if (path == NULL) {
        *error_code = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (!read_exact(worker->output, path,
            response.character_count * sizeof(wchar_t)) ||
        path[response.character_count - 1] != L'\0') {
        free(path);
        *error_code = ERROR_INVALID_DATA;
        resolve_worker_stop(worker);
        return NULL;
    }
    return path;
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
    finding->classification = DIAGNOSTIC_BLOCKING_CANDIDATE;
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

static int process_image_matches_target(
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    const wchar_t *image)
{
    const VolumeInfo *selected;
    size_t volume_index;
    size_t mount_index;
    if (image == NULL) return 0;
    selected = &inventory->volumes[target->volume_index];
    for (volume_index = 0; volume_index < inventory->count; volume_index++) {
        const VolumeInfo *volume = &inventory->volumes[volume_index];
        if (volume->removal_devinst != selected->removal_devinst) continue;
        for (mount_index = 0; mount_index < volume->mount_count; mount_index++) {
            if (text_path_is_at_or_below(image, volume->mount_points[mount_index])) return 1;
        }
    }
    return 0;
}

static int report_has_process_finding(
    const DiagnosticReport *report,
    DWORD pid,
    DiagnosticClassification classification)
{
    size_t index;
    for (index = 0; index < report->finding_count; index++) {
        if (report->findings[index].pid == pid &&
            report->findings[index].classification == classification) return 1;
    }
    return 0;
}

static int append_process_on_device(
    DiagnosticReport *report,
    const ProcessCacheEntry *process)
{
    BlockerFinding *new_items;
    size_t new_capacity;
    BlockerFinding *finding;
    if (report_has_process_finding(report, process->pid,
            DIAGNOSTIC_PROCESS_ON_DEVICE)) return 1;
    if (report->finding_count == report->finding_capacity) {
        new_capacity = report->finding_capacity == 0 ? 8 : report->finding_capacity * 2;
        if (new_capacity < report->finding_capacity ||
            new_capacity > ((size_t)-1) / sizeof(BlockerFinding)) return 0;
        new_items = (BlockerFinding *)realloc(report->findings,
            new_capacity * sizeof(BlockerFinding));
        if (new_items == NULL) return 0;
        report->findings = new_items;
        report->finding_capacity = new_capacity;
    }
    finding = &report->findings[report->finding_count];
    memset(finding, 0, sizeof(*finding));
    finding->classification = DIAGNOSTIC_PROCESS_ON_DEVICE;
    finding->pid = process->pid;
    finding->session_id = process->session_id;
    finding->process_creation_time = process->creation_time;
    finding->process_image = wide_duplicate(process->image);
    finding->process_user = wide_duplicate(process->user);
    finding->dos_path = wide_duplicate(process->image);
    finding->nt_path = wide_duplicate(L"");
    if (finding->process_image == NULL || finding->process_user == NULL ||
        finding->dos_path == NULL || finding->nt_path == NULL) {
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
        report_inspection_error(report, GetLastError(), L"enumerate-services");
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
        report_inspection_error(report, GetLastError(), L"enumerate-services");
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
    int path_timed_out;
    DWORD pid;
    ULONG_PTR calibration_value;
    ResolveWorker resolve_worker;
    HANDLE process_snapshot;
    PROCESSENTRY32W process_entry;

    app_error_clear(error);
    diagnostic_report_init(report);
    memset(&cache, 0, sizeof(cache));
    memset(&resolve_worker, 0, sizeof(resolve_worker));
    report->debug_privilege_enabled = enable_debug_privilege();

    if (GetModuleFileNameW(NULL, module, 32768) == 0) {
        if (error != NULL) {
            error->status = APP_DIAGNOSTIC_INCOMPLETE;
            error->win32_error = GetLastError();
            error->operation = L"locate diagnostic executable";
        }
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report_inspection_error(report, error != NULL ? error->win32_error : GetLastError(),
            L"locate-diagnostic-executable");
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
        report_inspection_error(report, error != NULL ? error->win32_error : GetLastError(),
            L"open-calibration-file");
        report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
        return APP_DIAGNOSTIC_INCOMPLETE;
    }

    buffer = capture_snapshot(&bytes, error);
    if (buffer == NULL) {
        CloseHandle(calibration);
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report_inspection_error(report, error != NULL ? error->win32_error : 0,
            L"capture-handle-snapshot");
        report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
        return APP_DIAGNOSTIC_INCOMPLETE;
    }
    snapshot = (NativeHandleSnapshot *)buffer;
    if (!snapshot_valid(snapshot, bytes)) {
        free(buffer);
        CloseHandle(calibration);
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report_inspection_error(report, ERROR_INVALID_DATA,
            L"validate-handle-snapshot");
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
        report_inspection_error(report, ERROR_INVALID_DATA,
            L"calibrate-file-object-type");
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
                report_inspection_error(report, process->open_error, L"open-process");
                report->inaccessible_process_count++;
                process->access_issue_counted = 1;
            }
            continue;
        }
        duplicate = NULL;
        if (!DuplicateHandle(process->handle, (HANDLE)native->handle_value,
                GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            report->issue_flags |= DIAG_ISSUE_CHANGED;
            report_inspection_error(report, GetLastError(), L"duplicate-handle");
            report->changed_handle_count++;
            continue;
        }
        if (GetFileType(duplicate) != FILE_TYPE_DISK) {
            CloseHandle(duplicate);
            continue;
        }
        report->inspected_file_handle_count++;
        path_error = ERROR_SUCCESS;
        path_timed_out = 0;
        path = query_nt_file_path(&resolve_worker, duplicate,
            &path_error, &path_timed_out);
        CloseHandle(duplicate);
        if (path == NULL) {
            if (path_timed_out) {
                report->issue_flags |= DIAG_ISSUE_TIMEOUT;
                report_inspection_error(report, WAIT_TIMEOUT, L"resolve-file-path");
            } else if (path_error == ERROR_INVALID_HANDLE) {
                report->issue_flags |= DIAG_ISSUE_CHANGED;
                report_inspection_error(report, path_error, L"resolve-file-path");
                report->changed_handle_count++;
            } else if (path_error == ERROR_ACCESS_DENIED) {
                report->issue_flags |= DIAG_ISSUE_ACCESS;
                report_inspection_error(report, path_error, L"resolve-file-path");
            } else if (path_error != ERROR_SUCCESS) {
                report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
                report_inspection_error(report, path_error, L"resolve-file-path");
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

    resolve_worker_stop(&resolve_worker);
    process_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (process_snapshot == INVALID_HANDLE_VALUE) {
        report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
        report_inspection_error(report, GetLastError(), L"enumerate-processes");
    } else {
        memset(&process_entry, 0, sizeof(process_entry));
        process_entry.dwSize = sizeof(process_entry);
        if (Process32FirstW(process_snapshot, &process_entry)) {
            do {
                process = process_cache_get(&cache, process_entry.th32ProcessID);
                if (process == NULL) {
                    report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
                    break;
                }
            } while (Process32NextW(process_snapshot, &process_entry));
        } else {
            report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
            report_inspection_error(report, GetLastError(), L"enumerate-processes");
        }
        CloseHandle(process_snapshot);
    }
    for (index = 0; index < cache.count; index++) {
        if (cache.items[index].handle != NULL &&
            process_image_matches_target(inventory, target, cache.items[index].image) &&
            !append_process_on_device(report, &cache.items[index])) {
            report->issue_flags |= DIAG_ISSUE_UNRESOLVED;
            break;
        }
    }
    attach_service_names(report);
    diagnostic_sort_report(report);
    process_cache_dispose(&cache);
    free(buffer);
    CloseHandle(calibration);
    report->completeness = diagnostic_completeness_from_issues(report->issue_flags);
    return report->completeness == DIAGNOSTIC_COMPLETE
        ? APP_OK : APP_DIAGNOSTIC_INCOMPLETE;
}
