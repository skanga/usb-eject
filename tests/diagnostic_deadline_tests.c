#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include "../src/diagnose.h"
static DWORD ticks;
static DWORD WINAPI fake_tick(void) { return ticks++; }
#define GetTickCount fake_tick
#include "../src/diagnose.c"
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL deadline line %d: %s\n", __LINE__, #x); failures++; } } while (0)
int main(void) {
    HANDLE reader, writer;
    DWORD error = 0, written;
    int timed_out = 0;
    char buffer[4];
    DeviceInventory inventory;
    VolumeInfo volume;
    ResolvedTarget target;
    DiagnosticReport report;
    AppError app_error;
    CHECK(CreatePipe(&reader, &writer, NULL, 0));
    ticks = 0;
    CHECK(!read_with_deadline(reader, buffer, sizeof(buffer), 0, 3, &timed_out, &error));
    CHECK(timed_out && error == WAIT_TIMEOUT);
    CHECK(WriteFile(writer, "ab", 2, &written, NULL));
    ticks = 0; timed_out = 0;
    CHECK(!read_with_deadline(reader, buffer, sizeof(buffer), 0, 3, &timed_out, &error));
    CHECK(timed_out && buffer[0] == 'a' && buffer[1] == 'b');
    CloseHandle(reader); CloseHandle(writer);
    memset(&inventory, 0, sizeof(inventory));
    memset(&volume, 0, sizeof(volume));
    memset(&target, 0, sizeof(target));
    volume.native_path = L"\\Device\\ReviewVolume";
    inventory.volumes = &volume; inventory.count = 1;
    ticks = 0;
    diagnostic_configure(1, 0);
    CHECK(diagnostic_scan(&inventory, &target, &report, &app_error) == APP_DIAGNOSTIC_INCOMPLETE);
    CHECK((report.issue_flags & DIAG_ISSUE_TIMEOUT) != 0);
    CHECK(report.last_operation != NULL && wcscmp(report.last_operation, L"scan-budget-exhausted") == 0);
    diagnostic_report_dispose(&report);
    printf("deadline tests: %d failures\n", failures);
    return failures != 0;
}
