#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string.h>
static int redirected_stdout;
static BOOL WINAPI fake_console(HANDLE handle, LPDWORD mode) {
    *mode = 0;
    return !(redirected_stdout && handle == GetStdHandle(STD_OUTPUT_HANDLE));
}
static BOOL WINAPI fake_read(HANDLE handle, LPVOID buffer, DWORD count, LPDWORD read, LPVOID control) {
    (void)handle; (void)count; (void)control;
    ((wchar_t *)buffer)[0] = L'y';
    ((wchar_t *)buffer)[1] = L'\r';
    ((wchar_t *)buffer)[2] = L'\n';
    *read = 3;
    return TRUE;
}
#define GetConsoleMode fake_console
#define ReadConsoleW fake_read
#define main review_original_main
#define inventory_build review_inventory_build
#define inventory_dispose review_inventory_dispose
#define diagnostic_scan review_diagnostic_scan
#define eject_parent_device review_eject_parent_device
#define action_stop_blocker review_action_stop_blocker
#define eject_card_target review_eject_card_target
#include "../src/main.c"
#undef main
#include <stdio.h>

static int scan_count;
static int eject_count;
static int partial_scan;
static int multiple_blockers;
static int acted_count;
static int remaining_blockers;
static int incomplete_inventory;
static int stop_failure;
static int force_count;
static int unsafe_scan;
static VolumeInfo review_volumes[2];
static wchar_t *review_mount0[] = { L"E:\\" };
static wchar_t *review_mount1[] = { L"F:\\" };

AppStatus review_inventory_build(DeviceInventory *inventory, AppError *error) {
    app_error_clear(error);
    memset(review_volumes, 0, sizeof(review_volumes));
    review_volumes[0].mount_points = review_mount0;
    review_volumes[1].mount_points = review_mount1;
    review_volumes[0].mount_count = review_volumes[1].mount_count = 1;
    review_volumes[0].removal_devinst = review_volumes[1].removal_devinst = 100;
    review_volumes[0].disk_devinst = 101;
    review_volumes[1].disk_devinst = 102;
    review_volumes[0].label = L"Photos";
    review_volumes[1].label = L"Archive";
    review_volumes[0].product = review_volumes[1].product = L"Review Reader";
    inventory->volumes = review_volumes;
    inventory->count = 2;
    if (incomplete_inventory) {
        inventory->skipped_transient = 1;
        error->status = APP_DIAGNOSTIC_INCOMPLETE;
        error->operation = L"test discovery failure";
        error->win32_error = ERROR_ACCESS_DENIED;
        return APP_DIAGNOSTIC_INCOMPLETE;
    }
    return APP_OK;
}

void review_inventory_dispose(DeviceInventory *inventory) {
    memset(inventory, 0, sizeof(*inventory));
}

AppStatus review_diagnostic_scan(const DeviceInventory *inventory,
    const ResolvedTarget *target, DiagnosticReport *report, AppError *error) {
    (void)inventory;
    (void)target;
    app_error_clear(error);
    diagnostic_report_init(report);
    report->completeness = DIAGNOSTIC_COMPLETE;
    scan_count++;
    if (unsafe_scan && scan_count == 2) report->issue_flags = DIAG_ISSUE_TIMEOUT;
    if (acted_count < (multiple_blockers ? 2 : 1) || remaining_blockers) {
        size_t index;
        report->finding_count = (multiple_blockers ? 2 : 1) - acted_count;
        if (remaining_blockers && report->finding_count == 0) report->finding_count = 1;
        report->findings = (BlockerFinding *)calloc(report->finding_count, sizeof(BlockerFinding));
        report->finding_capacity = report->finding_count;
        for (index = 0; index < report->finding_count; index++) {
            report->findings[index].pid = (DWORD)(123 + 333 * (index + acted_count));
            report->findings[index].classification = DIAGNOSTIC_BLOCKING_CANDIDATE;
        }
    } else if (partial_scan) {
        report->completeness = DIAGNOSTIC_PARTIAL_ACCESS;
        report->issue_flags = DIAG_ISSUE_ACCESS;
    }
    return report->completeness == DIAGNOSTIC_COMPLETE ? APP_OK : APP_DIAGNOSTIC_INCOMPLETE;
}

AppStatus review_eject_parent_device(DEVINST devinst, EjectResult *result) {
    (void)devinst;
    memset(result, 0, sizeof(*result));
    eject_count++;
    result->status = eject_count == 1 ? APP_BUSY : APP_OK;
    result->veto_type = PNP_VetoOutstandingOpen;
    return result->status;
}

AppStatus review_eject_card_target(const DeviceInventory *inventory,
    const ResolvedTarget *target, EjectResult *result) {
    (void)inventory; (void)target;
    return review_eject_parent_device(100, result);
}

AppStatus review_action_stop_blocker(const BlockerFinding *finding,
    int force, BlockerActionResult *result) {
    (void)finding;
    memset(result, 0, sizeof(*result));
    if (stop_failure && !force) {
        result->win32_error = ERROR_CANCELLED;
        return APP_BLOCKER_ACTION_FAILED;
    }
    if (force) force_count++;
    result->graceful_close_attempted = 1;
    acted_count++;
    return APP_OK;
}

int main(int argc, char **argv) {
    Command command;
    DWORD pid = 123;
    int status;
    DeviceInventory inventory;
    ResolvedTarget target;
    AppError error;
    TargetSelector selector;
    command_init(&command);
    if (argc > 1 && (strcmp(argv[1], "grouped") == 0 || strcmp(argv[1], "verbose") == 0 ||
        strcmp(argv[1], "diagnostic-tsv") == 0)) {
        DiagnosticReport report;
        BlockerFinding findings[4];
        size_t index;
        memset(findings, 0, sizeof(findings));
        diagnostic_report_init(&report);
        report.findings = findings; report.finding_count = 4; report.completeness = DIAGNOSTIC_COMPLETE;
        for (index = 0; index < 4; index++) {
            findings[index].pid = 123;
            findings[index].process_image = L"C:\\Editor.exe";
            findings[index].dos_path = L"E:\\Photos\\image.jpg";
            findings[index].nt_path = L"\\Device\\ReviewVolume\\Photos\\image.jpg";
            findings[index].handle_value = index + 1;
        }
        output_set_verbose(strcmp(argv[1], "verbose") == 0);
        return !output_diagnostic(&report, NULL,
            strcmp(argv[1], "diagnostic-tsv") == 0 ? OUTPUT_TSV : OUTPUT_TEXT, 0);
    }
    if (argc > 1 && strcmp(argv[1], "recovery") == 0) {
        review_inventory_build(&inventory, &error);
        review_mount0[0] = L"E:\\O'Brien & Photos\\";
        command.card_mode = command.quiet = command.no_prompt = command.verbose = 1;
        command.format = OUTPUT_TSV;
        return !output_recovery_command(&command, &review_volumes[0], 123);
    }
    if (argc > 1 && strcmp(argv[1], "card-target") == 0) {
        review_inventory_build(&inventory, &error);
        selector.kind = SELECTOR_LETTER; selector.value = L"E";
        status = target_resolve_mode(&inventory, &selector, 1, &target, &error);
        command.kind = COMMAND_EJECT; command.card_mode = 1;
        if (status != APP_OK) return 1;
        output_target_operation(&inventory, &target, &command);
        resolved_target_dispose(&target);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "redirect") == 0) {
        redirected_stdout = 1;
        if (interactive_console()) return 1;
        redirected_stdout = 0;
        if (!interactive_console()) return 1;
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "media-ambiguity") == 0) {
        review_inventory_build(&inventory, &error);
        selector.kind = SELECTOR_NAME;
        selector.value = L"Review Reader";
        status = target_resolve_mode(&inventory, &selector, 1, &target, &error);
        if (status != APP_AMBIGUOUS || target.distinct_match_count != 2) return 1;
        resolved_target_dispose(&target);
        review_volumes[1].disk_devinst = review_volumes[0].disk_devinst;
        status = target_resolve_mode(&inventory, &selector, 1, &target, &error);
        if (status != APP_OK || target.sibling_volume_count != 2) return 1;
        resolved_target_dispose(&target);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "target") == 0) {
        review_inventory_build(&inventory, &error);
        selector.kind = SELECTOR_NAME;
        selector.value = L"Review Reader";
        status = target_resolve(&inventory, &selector, &target, &error);
        printf("Resolution: status=%d distinct=%u selected=%u siblings=%u\n",
            status, (unsigned)target.distinct_match_count,
            (unsigned)target.volume_index, (unsigned)target.sibling_volume_count);
        output_target(&inventory, &target);
        resolved_target_dispose(&target);
        return 0;
    }
    partial_scan = argc > 1 && strcmp(argv[1], "partial") == 0;
    multiple_blockers = argc > 1 && strcmp(argv[1], "multiple") == 0;
    remaining_blockers = argc > 1 && strcmp(argv[1], "remaining") == 0;
    incomplete_inventory = argc > 1 && strcmp(argv[1], "inventory") == 0;
    stop_failure = argc > 1 && strcmp(argv[1], "force") == 0;
    unsafe_scan = argc > 1 && strcmp(argv[1], "unsafe-scan") == 0;
    command.kind = COMMAND_EJECT;
    command.selector.kind = SELECTOR_LETTER;
    command.selector.value = L"E";
    command.quiet = 1;
    command.no_prompt = 1;
    command.assume_yes = 1;
    command.authorized_pids = &pid;
    command.authorized_pid_count = 1;
    if (multiple_blockers) {
        command.authorized_pid_count = 0;
        command.no_prompt = 0;
    }
    status = run_inventory_command(&command, NULL, NULL);
    fprintf(stderr, "Review result: exit=%d eject_calls=%d scans=%d\n",
        status, eject_count, scan_count);
    if (incomplete_inventory) return status != APP_DIAGNOSTIC_INCOMPLETE || eject_count != 0;
    if (unsafe_scan) return status != APP_BLOCKER_ACTION_FAILED || acted_count != 0 || eject_count != 1;
    if (remaining_blockers) return status != APP_BUSY || eject_count != 1;
    if (multiple_blockers && acted_count != 2) return 1;
    if (stop_failure && force_count != 1) return 1;
    return status != APP_OK || eject_count != 2;
}
