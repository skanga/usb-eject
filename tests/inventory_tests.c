#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include "../src/inventory.h"
static DWORD open_error = ERROR_ACCESS_DENIED;
static HANDLE WINAPI fake_open(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    (void)path; (void)access; (void)share; (void)security; (void)creation; (void)flags; (void)template_file;
    SetLastError(open_error);
    return INVALID_HANDLE_VALUE;
}
static HDEVINFO WINAPI fake_interfaces(const GUID *guid, PCWSTR enumerator, HWND parent, DWORD flags) {
    (void)guid; (void)enumerator; (void)parent; (void)flags;
    SetLastError(ERROR_ACCESS_DENIED);
    return INVALID_HANDLE_VALUE;
}
#define CreateFileW fake_open
#define SetupDiGetClassDevsW fake_interfaces
#include "../src/inventory.c"
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL inventory line %d: %s\n", __LINE__, #x); failures++; } } while (0)
int main(void) {
    DeviceInventory inventory;
    AppError error;
    inventory_init(&inventory);
    CHECK(scan_volume(&inventory, L"\\\\?\\Volume{test}\\", &error) == APP_OK);
    CHECK(inventory.skipped_transient == 1 && inventory.last_error == ERROR_ACCESS_DENIED);
    inventory_dispose(&inventory);
    open_error = ERROR_NO_MEDIA_IN_DRIVE;
    CHECK(scan_volume(&inventory, L"\\\\?\\Volume{test}\\", &error) == APP_OK);
    CHECK(inventory.skipped_transient == 0);
    map_device_interfaces(&inventory);
    CHECK(inventory.skipped_transient == 1 && inventory.last_error == ERROR_ACCESS_DENIED);
    inventory_dispose(&inventory);
    CHECK(inventory_build(&inventory, &error) == APP_DIAGNOSTIC_INCOMPLETE);
    CHECK(error.status == APP_DIAGNOSTIC_INCOMPLETE && error.operation != NULL);
    inventory_dispose(&inventory);
    printf("inventory tests: %d failures\n", failures);
    return failures != 0;
}
