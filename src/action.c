#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "action.h"
#include "text.h"
#include "win32_compat.h"

typedef BOOL (WINAPI *IsProcessCriticalFn)(HANDLE, PBOOL);

typedef struct {
    DWORD pid;
    unsigned windows_found;
} CloseWindowsContext;

int action_pid_authorized(const Command *command, DWORD pid) {
    size_t index;
    if (command == NULL) return 0;
    for (index = 0; index < command->authorized_pid_count; index++) {
        if (command->authorized_pids[index] == pid) return 1;
    }
    return 0;
}

static ULONGLONG filetime_value(const FILETIME *value) {
    return ((ULONGLONG)value->dwHighDateTime << 32) | value->dwLowDateTime;
}

static int process_identity_matches(HANDLE process, const BlockerFinding *finding) {
    FILETIME creation;
    FILETIME exit_time;
    FILETIME kernel;
    FILETIME user;
    wchar_t *image;
    DWORD capacity;
    int result;

    if (!GetProcessTimes(process, &creation, &exit_time, &kernel, &user) ||
        filetime_value(&creation) != finding->process_creation_time) return 0;
    capacity = 32768;
    image = (wchar_t *)calloc(capacity, sizeof(wchar_t));
    if (image == NULL) return 0;
    if (!QueryFullProcessImageNameW(process, 0, image, &capacity)) {
        free(image);
        return 0;
    }
    result = text_iequals(image, finding->process_image);
    free(image);
    return result;
}

static int process_is_known_noncritical(HANDLE process) {
    HMODULE kernel32;
    IsProcessCriticalFn query;
    BOOL critical;

    kernel32 = GetModuleHandleW(L"kernel32.dll");
    query = kernel32 != NULL
        ? (IsProcessCriticalFn)GetProcAddress(kernel32, "IsProcessCritical")
        : NULL;
    if (query == NULL) return 0;
    critical = TRUE;
    if (!query(process, &critical)) return 0;
    return !critical;
}

static BOOL CALLBACK close_window_callback(HWND window, LPARAM parameter) {
    CloseWindowsContext *context;
    DWORD pid;
    DWORD_PTR message_result;

    context = (CloseWindowsContext *)parameter;
    pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != context->pid) return TRUE;
    context->windows_found++;
    message_result = 0;
    SendMessageTimeoutW(window, WM_CLOSE, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 750, &message_result);
    return TRUE;
}

static int stop_single_service(const wchar_t *service_name, DWORD *error_code) {
    SC_HANDLE manager;
    SC_HANDLE service;
    SERVICE_STATUS_PROCESS status;
    SERVICE_STATUS basic_status;
    DWORD needed;
    DWORD started;

    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (manager == NULL) {
        *error_code = GetLastError();
        return 0;
    }
    service = OpenServiceW(manager, service_name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service == NULL) {
        *error_code = GetLastError();
        CloseServiceHandle(manager);
        return 0;
    }
    needed = 0;
    memset(&status, 0, sizeof(status));
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            (LPBYTE)&status, sizeof(status), &needed)) {
        *error_code = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 0;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 1;
    }
    if ((status.dwControlsAccepted & SERVICE_ACCEPT_STOP) == 0) {
        *error_code = ERROR_ACCESS_DENIED;
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 0;
    }
    memset(&basic_status, 0, sizeof(basic_status));
    if (!ControlService(service, SERVICE_CONTROL_STOP, &basic_status)) {
        *error_code = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 0;
    }
    started = GetTickCount();
    for (;;) {
        Sleep(100);
        needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                (LPBYTE)&status, sizeof(status), &needed)) {
            *error_code = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 0;
        }
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        if (status.dwCurrentState != SERVICE_STOP_PENDING ||
            GetTickCount() - started >= 5000) {
            *error_code = WAIT_TIMEOUT;
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 0;
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return 1;
}

AppStatus action_stop_blocker(
    const BlockerFinding *finding,
    int allow_forced_termination,
    BlockerActionResult *result)
{
    HANDLE process;
    CloseWindowsContext close_context;
    DWORD wait_result;
    DWORD error;

    memset(result, 0, sizeof(*result));
    if (finding == NULL || finding->pid == 0 || finding->pid == 4 ||
        finding->pid == GetCurrentProcessId() || finding->process_creation_time == 0 ||
        finding->process_image == NULL || finding->service_count > 1) {
        result->win32_error = ERROR_ACCESS_DENIED;
        return APP_BLOCKER_ACTION_FAILED;
    }

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, finding->pid);
    if (process == NULL) {
        error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) return APP_OK;
        result->win32_error = error;
        return APP_BLOCKER_ACTION_FAILED;
    }
    if (!process_identity_matches(process, finding) ||
        !process_is_known_noncritical(process)) {
        CloseHandle(process);
        result->win32_error = ERROR_ACCESS_DENIED;
        return APP_BLOCKER_ACTION_FAILED;
    }

    if (finding->service_count == 1) {
        result->graceful_close_attempted = 1;
        CloseHandle(process);
        if (stop_single_service(finding->services[0], &result->win32_error)) {
            return APP_OK;
        }
        return APP_BLOCKER_ACTION_FAILED;
    }

    memset(&close_context, 0, sizeof(close_context));
    close_context.pid = finding->pid;
    EnumWindows(close_window_callback, (LPARAM)&close_context);
    if (close_context.windows_found != 0) {
        result->graceful_close_attempted = 1;
        wait_result = WaitForSingleObject(process, 3000);
        if (wait_result == WAIT_OBJECT_0) {
            CloseHandle(process);
            return APP_OK;
        }
    }
    CloseHandle(process);

    if (!allow_forced_termination) {
        result->win32_error = ERROR_CANCELLED;
        return APP_BLOCKER_ACTION_FAILED;
    }

    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
        PROCESS_TERMINATE | SYNCHRONIZE, FALSE, finding->pid);
    if (process == NULL) {
        error = GetLastError();
        if (error == ERROR_INVALID_PARAMETER) return APP_OK;
        result->win32_error = error;
        return APP_BLOCKER_ACTION_FAILED;
    }
    if (!process_identity_matches(process, finding) ||
        !process_is_known_noncritical(process)) {
        CloseHandle(process);
        result->win32_error = ERROR_ACCESS_DENIED;
        return APP_BLOCKER_ACTION_FAILED;
    }
    if (!TerminateProcess(process, APP_BLOCKER_ACTION_FAILED)) {
        result->win32_error = GetLastError();
        CloseHandle(process);
        return APP_BLOCKER_ACTION_FAILED;
    }
    result->forced_termination_used = 1;
    wait_result = WaitForSingleObject(process, 5000);
    CloseHandle(process);
    if (wait_result != WAIT_OBJECT_0) {
        result->win32_error = wait_result == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
        return APP_BLOCKER_ACTION_FAILED;
    }
    return APP_OK;
}
