#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../src/diagnose.h"

int main(void) {
    wchar_t source_path[32768];
    wchar_t drive_name[3];
    wchar_t native_path[1024];
    wchar_t root[4];
    wchar_t *mounts[1];
    HANDLE held_file;
    VolumeInfo volume;
    DeviceInventory inventory;
    ResolvedTarget target;
    DiagnosticReport report;
    AppError error;
    size_t index;
    int found;

    if (GetFullPathNameW(L".\\tests\\test_main.c", 32768, source_path, NULL) == 0) {
        printf("cannot resolve smoke-test file\n");
        return 1;
    }
    drive_name[0] = source_path[0];
    drive_name[1] = L':';
    drive_name[2] = L'\0';
    if (QueryDosDeviceW(drive_name, native_path, 1024) == 0) {
        printf("cannot resolve native volume path\n");
        return 1;
    }
    root[0] = source_path[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';
    mounts[0] = root;

    held_file = CreateFileW(source_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, 0, NULL);
    if (held_file == INVALID_HANDLE_VALUE) {
        printf("cannot open smoke-test file\n");
        return 1;
    }

    memset(&volume, 0, sizeof(volume));
    volume.native_path = native_path;
    volume.mount_points = mounts;
    volume.mount_count = 1;
    volume.removal_devinst = 1234;
    memset(&inventory, 0, sizeof(inventory));
    inventory.volumes = &volume;
    inventory.count = 1;
    memset(&target, 0, sizeof(target));
    target.volume_index = 0;
    target.removal_devinst = 1234;

    diagnostic_report_init(&report);
    diagnostic_scan(&inventory, &target, &report, &error);
    found = 0;
    for (index = 0; index < report.finding_count; index++) {
        if (report.findings[index].pid == GetCurrentProcessId() &&
            report.findings[index].handle_value == (ULONG_PTR)held_file) {
            found = 1;
            break;
        }
    }
    diagnostic_report_dispose(&report);
    CloseHandle(held_file);

    if (!found) {
        printf("diagnostic scan did not find its known open file handle\n");
        return 1;
    }
    printf("diagnostic smoke test passed\n");
    return 0;
}
