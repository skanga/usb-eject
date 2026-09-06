#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include "../src/eject.h"
#ifndef FSCTL_LOCK_VOLUME
#define FSCTL_LOCK_VOLUME 0x00090018
#endif

static int locks, flushes, ejects, closes, failures;
static DWORD failure_code;
static DWORD failure_operation;
static int fail_flush;
static HANDLE WINAPI fake_open(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD creation, DWORD flags, HANDLE template_file) {
    (void)path; (void)access; (void)share; (void)security; (void)creation; (void)flags; (void)template_file;
    return (HANDLE)123;
}
static BOOL WINAPI fake_close(HANDLE handle) { (void)handle; closes++; return TRUE; }
static BOOL WINAPI fake_flush(HANDLE handle) {
    (void)handle; flushes++;
    if (fail_flush) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return TRUE;
}
static BOOL WINAPI fake_ioctl(HANDLE handle, DWORD operation, LPVOID input, DWORD input_size,
    LPVOID output, DWORD output_size, LPDWORD returned, LPOVERLAPPED overlapped) {
    (void)handle; (void)input; (void)input_size; (void)output; (void)output_size; (void)overlapped;
    *returned = 0;
    if (operation == FSCTL_LOCK_VOLUME) locks++;
    if (operation == IOCTL_STORAGE_EJECT_MEDIA) {
        ejects++;
        if (locks == 0 || flushes == 0) failures++;
    }
    if (operation == failure_operation) { SetLastError(failure_code); return FALSE; }
    return TRUE;
}
#define CreateFileW fake_open
#define CloseHandle fake_close
#define FlushFileBuffers fake_flush
#define DeviceIoControl fake_ioctl
#include "../src/eject.c"

#define CHECK(x) do { if (!(x)) { printf("FAIL card line %d: %s\n", __LINE__, #x); failures++; } } while (0)
int main(void) {
    VolumeInfo volume;
    EjectResult result;
    DeviceInventory inventory;
    ResolvedTarget target;
    VolumeInfo volumes[2];
    memset(&volume, 0, sizeof(volume));
    CHECK(eject_card_media(NULL, &result) == APP_UNSUPPORTED);
    volume.volume_guid = L"\\\\?\\Volume{test}\\";
    CHECK(eject_card_media(&volume, &result) == APP_OK);
    CHECK(locks == 1 && flushes == 1 && ejects == 1 && closes == 1);
    locks = flushes = ejects = closes = 0;
    failure_operation = FSCTL_LOCK_VOLUME;
    failure_code = ERROR_ACCESS_DENIED;
    CHECK(eject_card_media(&volume, &result) == APP_BUSY);
    CHECK(ejects == 0 && closes == 1);
    failure_operation = IOCTL_STORAGE_CHECK_VERIFY2;
    failure_code = ERROR_NOT_SUPPORTED;
    CHECK(eject_card_media(&volume, &result) == APP_UNSUPPORTED);
    failure_code = ERROR_CRC;
    CHECK(eject_card_media(&volume, &result) == APP_INTERNAL_ERROR);
    failure_operation = 0;
    fail_flush = 1;
    ejects = 0;
    CHECK(eject_card_media(&volume, &result) == APP_INTERNAL_ERROR && ejects == 0);
    fail_flush = 0;
    volumes[0] = volumes[1] = volume;
    volumes[0].disk_devinst = volumes[1].disk_devinst = 101;
    memset(&inventory, 0, sizeof(inventory));
    memset(&target, 0, sizeof(target));
    inventory.volumes = volumes; inventory.count = 2; target.media_scope = 1;
    locks = flushes = ejects = closes = 0;
    CHECK(eject_card_target(&inventory, &target, &result) == APP_OK);
    CHECK(locks == 2 && flushes == 2 && ejects == 1 && closes == 2);
    printf("card tests: %d failures\n", failures);
    return failures != 0;
}
