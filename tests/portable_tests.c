#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../src/portable.h"
static wchar_t test_temp[32768];
static wchar_t child_directory[32768];
static wchar_t child_executable[32768];
static wchar_t child_command[32768];
static int launches;
static int fail_launch;
static DWORD WINAPI fake_temp(DWORD capacity, LPWSTR buffer) {
    if (wcslen(test_temp) >= capacity) return 0;
    wcscpy(buffer, test_temp);
    return (DWORD)wcslen(buffer);
}
static BOOL WINAPI fake_launch(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES process_security,
    LPSECURITY_ATTRIBUTES thread_security, BOOL inherit, DWORD flags, LPVOID environment,
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    (void)process_security; (void)thread_security; (void)inherit; (void)flags; (void)environment; (void)startup;
    launches++;
    wcscpy(child_directory, directory != NULL ? directory : L"");
    wcscpy(child_executable, application);
    wcscpy(child_command, command);
    if (fail_launch) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    memset(process, 0, sizeof(*process));
    process->hProcess = CreateEventW(NULL, TRUE, FALSE, NULL);
    process->hThread = CreateEventW(NULL, TRUE, FALSE, NULL);
    return TRUE;
}
#define GetTempPathW fake_temp
#define CreateProcessW fake_launch
#include "../src/portable.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL portable line %d: %s (error %lu)\n", __LINE__, #x, (unsigned long)GetLastError()); failures++; } } while (0)
int main(void) {
    wchar_t root[32768], receipt[32768], mount[32768], guid[MAX_PATH + 1];
    wchar_t *mounts[] = { L"Z:\\" };
    VolumeInfo volume;
    DeviceInventory inventory;
    ResolvedTarget target;
    Command command;
    AppError error;
    HANDLE file;
    char text[256];
    DWORD read;
    GetFullPathNameW(L"build", 32768, root, NULL);
    CHECK(CreateDirectoryW(root, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    if (failures) return 1;
    _snwprintf(test_temp, 32768, L"%ls\\portable-test-%lu-%lu\\", root,
        (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    CHECK(CreateDirectoryW(test_temp, NULL));
    _snwprintf(receipt, 32768, L"%lsresult.txt", test_temp);
    memset(&volume, 0, sizeof(volume));
    volume.mount_points = mounts;
    volume.mount_count = 1;
    volume.instance_id = L"REVIEW\\DEVICE";
    volume.volume_guid = L"\\\\?\\Volume{review-target}\\";
    memset(&inventory, 0, sizeof(inventory));
    inventory.volumes = &volume; inventory.count = 1;
    memset(&target, 0, sizeof(target));
    command_init(&command);
    command.selector.kind = SELECTOR_THIS;
    command.result_file = receipt;
    command.verbose = 1;
    command.scan_timeout_ms = 4321;
    CHECK(portable_launch(&command, &inventory, &target, &error) == APP_STARTED);
    CHECK(launches == 1 && wcsncmp(child_directory, test_temp, wcslen(test_temp)) == 0);
    CHECK(wcsstr(child_command, L"--result-file") != NULL && wcsstr(child_command, L"--scan-timeout 4321") != NULL);
    CHECK(portable_finish_result(receipt, APP_BUSY));
    memset(text, 0, sizeof(text));
    file = CreateFileW(receipt, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(ReadFile(file, text, sizeof(text) - 1, &read, NULL));
    CloseHandle(file);
    CHECK(strstr(text, "state=pending") != NULL && strstr(text, "exit_code=4") != NULL);
    CHECK(portable_launch(&command, &inventory, &target, &error) != APP_STARTED);
    CHECK(launches == 1);
    DeleteFileW(child_executable);
    RemoveDirectoryW(child_directory);
    DeleteFileW(receipt);
    fail_launch = 1;
    CHECK(portable_launch(&command, &inventory, &target, &error) == APP_ACCESS_DENIED);
    file = CreateFileW(receipt, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    memset(text, 0, sizeof(text));
    CHECK(ReadFile(file, text, sizeof(text) - 1, &read, NULL));
    CloseHandle(file);
    CHECK(strstr(text, "exit_code=5") != NULL);
    DeleteFileW(receipt);
    CHECK(GetVolumePathNameW(test_temp, mount, 32768));
    CHECK(GetVolumeNameForVolumeMountPointW(mount, guid, MAX_PATH + 1));
    volume.volume_guid = guid;
    CHECK(portable_launch(&command, &inventory, &target, &error) == APP_UNSUPPORTED);
    CHECK(launches == 2);
    RemoveDirectoryW(test_temp);
    printf("portable tests: %d failures\n", failures);
    return failures != 0;
}
