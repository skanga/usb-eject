#define UNICODE
#define _UNICODE

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../src/action.h"
#include "../src/cli.h"
#include "../src/diagnose.h"
#include "../src/eject.h"
#include "../src/inventory.h"
#include "../src/output.h"
#include "../src/portable.h"
#include "../src/target.h"
#include "../src/text.h"

__declspec(dllimport) LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR, int *);

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_case_insensitive_matching(void) {
    CHECK(text_iequals(L"LiSt", L"list"));
    CHECK(text_iequals(L"USB DISK", L"usb disk"));
    CHECK(!text_iequals(L"disk", L"diskette"));
}

static void test_supported_patterns(void) {
    CHECK(text_pattern_valid(L"SanDisk"));
    CHECK(text_pattern_valid(L"San*"));
    CHECK(text_pattern_valid(L"*Disk"));
    CHECK(text_pattern_valid(L"*disk*"));
    CHECK(!text_pattern_valid(L"San*Disk"));
    CHECK(!text_pattern_valid(L"**"));

    CHECK(text_match_pattern(L"SanDisk Ultra", L"San*"));
    CHECK(text_match_pattern(L"SanDisk Ultra", L"*ULTRA"));
    CHECK(text_match_pattern(L"SanDisk Ultra", L"*disk*"));
    CHECK(!text_match_pattern(L"SanDisk Ultra", L"King*"));
}

static void test_path_boundaries(void) {
    CHECK(text_path_is_at_or_below(
        L"\\Device\\HarddiskVolume7\\documents\\report.docx",
        L"\\Device\\HarddiskVolume7"));
    CHECK(text_path_is_at_or_below(
        L"\\Device\\HarddiskVolume7",
        L"\\Device\\HarddiskVolume7"));
    CHECK(!text_path_is_at_or_below(
        L"\\Device\\HarddiskVolume70\\report.docx",
        L"\\Device\\HarddiskVolume7"));
    CHECK(text_path_is_at_or_below(L"E:\\folder\\file.txt", L"e:\\"));
}

static void test_tsv_escaping(void) {
    wchar_t output[64];
    size_t needed;

    needed = text_tsv_escape(L"a\\b\tc\r\nd", output, 64);
    CHECK(needed == 12);
    CHECK(wcscmp(output, L"a\\\\b\\tc\\r\\nd") == 0);

    needed = text_tsv_escape(L"abcdef", output, 4);
    CHECK(needed == 6);
    CHECK(wcscmp(output, L"abc") == 0);
}

static void test_storage_descriptor_strings_are_bounds_checked(void) {
    unsigned char descriptor[64];
    wchar_t output[32];
    size_t index;

    for (index = 0; index < sizeof(descriptor); index++) descriptor[index] = 0;
    descriptor[40] = ' ';
    descriptor[41] = 'S';
    descriptor[42] = 'a';
    descriptor[43] = 'n';
    descriptor[44] = 'D';
    descriptor[45] = 'i';
    descriptor[46] = 's';
    descriptor[47] = 'k';
    descriptor[48] = ' ';

    CHECK(inventory_copy_descriptor_string(
        descriptor, sizeof(descriptor), 40, output, 32));
    CHECK(wcscmp(output, L"SanDisk") == 0);
    CHECK(!inventory_copy_descriptor_string(
        descriptor, sizeof(descriptor), 64, output, 32));

    for (index = 40; index < sizeof(descriptor); index++) descriptor[index] = 'X';
    CHECK(!inventory_copy_descriptor_string(
        descriptor, sizeof(descriptor), 40, output, 32));
}

static void test_cli_happy_paths(void) {
    Command command;
    ParseError error;
    wchar_t *list_args[] = { L"usb-eject.exe", L"LiSt", L"--format", L"tsv" };
    wchar_t *eject_args[] = {
        L"usb-eject.exe", L"eject", L"E:",
        L"--kill-blocker", L"8420", L"--yes"
    };

    command_init(&command);
    CHECK(cli_parse(4, list_args, &command, &error));
    CHECK(command.kind == COMMAND_LIST);
    CHECK(command.format == OUTPUT_TSV);
    command_dispose(&command);

    command_init(&command);
    CHECK(cli_parse(6, eject_args, &command, &error));
    CHECK(command.kind == COMMAND_EJECT);
    CHECK(command.selector.kind == SELECTOR_IMPLICIT);
    CHECK(wcscmp(command.selector.value, L"E:") == 0);
    CHECK(command.authorized_pid_count == 1);
    CHECK(command.authorized_pids[0] == 8420);
    CHECK(command.assume_yes);
    command_dispose(&command);
}

static void test_cli_conventional_aliases_and_letter_validation(void) {
    Command command;
    ParseError error;
    wchar_t *version_args[] = { L"usb-eject.exe", L"--version" };
    wchar_t *bad_letter[] = {
        L"usb-eject.exe", L"diagnose", L"--letter", L"12"
    };
    wchar_t *help_eject[] = { L"usb-eject.exe", L"help", L"eject" };
    wchar_t *list_help[] = { L"usb-eject.exe", L"list", L"--help" };

    command_init(&command);
    CHECK(cli_parse(2, version_args, &command, &error));
    CHECK(command.kind == COMMAND_VERSION);
    command_dispose(&command);

    command_init(&command);
    CHECK(cli_parse(3, help_eject, &command, &error));
    CHECK(command.kind == COMMAND_HELP);
    CHECK(command.help_topic == COMMAND_EJECT);
    command_dispose(&command);

    command_init(&command);
    CHECK(cli_parse(3, list_help, &command, &error));
    CHECK(command.kind == COMMAND_HELP);
    CHECK(command.help_topic == COMMAND_LIST);
    command_dispose(&command);

    command_init(&command);
    CHECK(!cli_parse(4, bad_letter, &command, &error));
    CHECK(error.argument_index == 3);
    CHECK(error.message != NULL && wcsstr(error.message, L"letter") != NULL);
    command_dispose(&command);
}

static void test_cli_rejects_unsafe_or_ambiguous_input(void) {
    Command command;
    ParseError error;
    wchar_t *conflict[] = {
        L"usb-eject.exe", L"eject", L"--letter", L"E", L"--mount", L"F:\\"
    };
    wchar_t *kill_on_list[] = {
        L"usb-eject.exe", L"list", L"--kill-blocker", L"42"
    };
    wchar_t *bad_pid[] = {
        L"usb-eject.exe", L"eject", L"E:", L"--kill-blocker", L"4x"
    };
    wchar_t *bad_pattern[] = {
        L"usb-eject.exe", L"eject", L"--name", L"San*Disk"
    };
    wchar_t *yes_without_pid[] = {
        L"usb-eject.exe", L"eject", L"E:", L"--yes"
    };

    command_init(&command);
    CHECK(!cli_parse(6, conflict, &command, &error));
    command_dispose(&command);

    command_init(&command);
    CHECK(!cli_parse(4, kill_on_list, &command, &error));
    command_dispose(&command);

    command_init(&command);
    CHECK(!cli_parse(5, bad_pid, &command, &error));
    command_dispose(&command);

    command_init(&command);
    CHECK(!cli_parse(5, bad_pattern, &command, &error));
    command_dispose(&command);

    command_init(&command);
    CHECK(!cli_parse(4, yes_without_pid, &command, &error));
    command_dispose(&command);
}

static void test_cli_help_and_missing_values(void) {
    Command command;
    ParseError error;
    wchar_t *help[] = { L"usb-eject.exe", L"eject", L"E:", L"--help" };
    wchar_t *missing[] = { L"usb-eject.exe", L"diagnose", L"--label", L"--no-prompt" };
    wchar_t *literal[] = { L"usb-eject.exe", L"diagnose", L"--label=--help" };
    wchar_t *format[] = { L"usb-eject.exe", L"list", L"--format=tsv" };
    wchar_t *timeout[] = { L"usb-eject.exe", L"list", L"--scan-timeout=15000" };
    command_init(&command);
    CHECK(cli_parse(4, help, &command, &error));
    CHECK(command.kind == COMMAND_HELP && command.help_topic == COMMAND_EJECT);
    command_dispose(&command);
    command_init(&command);
    CHECK(!cli_parse(4, missing, &command, &error));
    CHECK(error.argument_index == 2);
    command_dispose(&command);
    command_init(&command);
    CHECK(cli_parse(3, literal, &command, &error));
    CHECK(command.selector.value != NULL && wcscmp(command.selector.value, L"--help") == 0);
    command_dispose(&command);
    command_init(&command);
    CHECK(cli_parse(3, format, &command, &error));
    CHECK(command.format == OUTPUT_TSV);
    command_dispose(&command);
    command_init(&command);
    CHECK(!cli_parse(3, timeout, &command, &error));
    CHECK(error.argument_index == 2);
    command_dispose(&command);
}

static void test_target_resolution_collapses_sibling_volumes(void) {
    DeviceInventory inventory;
    VolumeInfo volumes[3];
    wchar_t *mounts0[] = { L"E:\\" };
    wchar_t *mounts1[] = { L"F:\\Archive\\" };
    wchar_t *mounts2[] = { L"G:\\" };
    TargetSelector selector;
    ResolvedTarget resolved;
    AppError error;

    memset(&inventory, 0, sizeof(inventory));
    memset(volumes, 0, sizeof(volumes));
    inventory.volumes = volumes;
    inventory.count = 3;

    volumes[0].label = L"Photos";
    volumes[0].product = L"Fast Disk";
    volumes[0].mount_points = mounts0;
    volumes[0].mount_count = 1;
    volumes[0].removal_devinst = 100;

    volumes[1].label = L"Archive";
    volumes[1].product = L"Fast Disk";
    volumes[1].mount_points = mounts1;
    volumes[1].mount_count = 1;
    volumes[1].removal_devinst = 100;

    volumes[2].label = L"Photos";
    volumes[2].product = L"Other Disk";
    volumes[2].mount_points = mounts2;
    volumes[2].mount_count = 1;
    volumes[2].removal_devinst = 200;

    selector.kind = SELECTOR_LETTER;
    selector.value = L"e";
    CHECK(target_resolve(&inventory, &selector, &resolved, &error) == APP_OK);
    CHECK(resolved.volume_index == 0);
    CHECK(resolved.removal_devinst == 100);
    CHECK(resolved.sibling_volume_count == 2);
    resolved_target_dispose(&resolved);

    selector.kind = SELECTOR_NAME;
    selector.value = L"Fast*";
    CHECK(target_resolve(&inventory, &selector, &resolved, &error) == APP_OK);
    CHECK(resolved.removal_devinst == 100);
    resolved_target_dispose(&resolved);

    selector.kind = SELECTOR_LABEL;
    selector.value = L"Photos";
    CHECK(target_resolve(&inventory, &selector, &resolved, &error) == APP_AMBIGUOUS);
    CHECK(resolved.distinct_match_count == 2);
    CHECK(resolved.distinct_volume_indexes != NULL);
    CHECK(resolved.distinct_volume_indexes[0] == 0);
    CHECK(resolved.distinct_volume_indexes[1] == 2);
    resolved_target_dispose(&resolved);
}

static void test_inventory_card_reader_classification_and_sorting(void) {
    DeviceInventory inventory;
    VolumeInfo volumes[2];
    wchar_t *mounts0[] = { L"Z:\\" };
    wchar_t *mounts1[] = { L"e:\\" };
    wchar_t *multiple_mounts[] = { L"Z:\\Folder", L"F:\\" };

    CHECK(inventory_card_reader_hint(1, L"Generic", L"USB SD Reader") == 1);
    CHECK(inventory_card_reader_hint(0, L"Samsung", L"Portable SSD") == 0);
    CHECK(inventory_card_reader_hint(1, L"Generic", L"Flash Disk") == -1);

    memset(&inventory, 0, sizeof(inventory));
    memset(volumes, 0, sizeof(volumes));
    inventory.volumes = volumes;
    inventory.count = 2;
    volumes[0].mount_points = mounts0;
    volumes[0].mount_count = 1;
    volumes[0].volume_guid = L"volume-z";
    volumes[1].mount_points = mounts1;
    volumes[1].mount_count = 1;
    volumes[1].volume_guid = L"volume-e";
    inventory_sort(&inventory);
    CHECK(inventory.volumes[0].mount_points == mounts1);
    CHECK(inventory.volumes[1].mount_points == mounts0);
    inventory.count = 1;
    inventory.volumes[0].mount_points = multiple_mounts;
    inventory.volumes[0].mount_count = 2;
    inventory_sort(&inventory);
    CHECK(wcscmp(inventory.volumes[0].mount_points[0], L"F:\\") == 0);
}

static void test_diagnostic_classification_names(void) {
    CHECK(wcscmp(diagnostic_classification_name(DIAGNOSTIC_BLOCKING_CANDIDATE),
        L"blocking-candidate") == 0);
    CHECK(wcscmp(diagnostic_classification_name(DIAGNOSTIC_PROCESS_ON_DEVICE),
        L"process-on-device") == 0);
    CHECK(wcscmp(diagnostic_classification_name(DIAGNOSTIC_CONFIRMED_VETO),
        L"confirmed-veto") == 0);
    CHECK(wcscmp(diagnostic_classification_name(DIAGNOSTIC_UNRESOLVED_FINDING),
        L"unresolved") == 0);
}

static void test_diagnostic_finding_sorting(void) {
    DiagnosticReport report;
    BlockerFinding findings[3];
    diagnostic_report_init(&report);
    memset(findings, 0, sizeof(findings));
    report.findings = findings;
    report.finding_count = 3;
    findings[0].pid = 20;
    findings[0].dos_path = L"Z:\\b";
    findings[1].pid = 10;
    findings[1].dos_path = L"Z:\\c";
    findings[2].pid = 10;
    findings[2].dos_path = L"Z:\\a";
    diagnostic_sort_report(&report);
    CHECK(report.findings[0].pid == 10);
    CHECK(wcscmp(report.findings[0].dos_path, L"Z:\\a") == 0);
    CHECK(wcscmp(report.findings[1].dos_path, L"Z:\\c") == 0);
    CHECK(report.findings[2].pid == 20);
    report.findings = NULL;
    report.finding_count = 0;
}

static void test_diagnostic_tsv_contains_veto_and_unresolved_rows(void) {
    DiagnosticReport report;
    EjectResult eject_result;
    HANDLE read_pipe;
    HANDLE write_pipe;
    HANDLE original_output;
    char captured[4096];
    DWORD read;
    diagnostic_report_init(&report);
    report.completeness = DIAGNOSTIC_PARTIAL_ACCESS;
    memset(&eject_result, 0, sizeof(eject_result));
    eject_result.status = APP_BUSY;
    eject_result.config_ret = 1;
    eject_result.veto_type = PNP_VetoOutstandingOpen;
    wcscpy(eject_result.veto_name, L"test-veto");
    CHECK(CreatePipe(&read_pipe, &write_pipe, NULL, 0));
    original_output = GetStdHandle(STD_OUTPUT_HANDLE);
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, write_pipe));
    CHECK(output_diagnostic(&report, &eject_result, OUTPUT_TSV, 0));
    CHECK(output_diagnostic_append(&report, &eject_result, OUTPUT_TSV, 0));
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, original_output));
    CloseHandle(write_pipe);
    memset(captured, 0, sizeof(captured));
    read = 0;
    CHECK(ReadFile(read_pipe, captured, sizeof(captured) - 1, &read, NULL));
    CloseHandle(read_pipe);
    CHECK(strstr(captured, "classification\tpid") != NULL);
    CHECK(strstr(strstr(captured, "classification\tpid") + 1,
        "classification\tpid") == NULL);
    CHECK(strstr(captured, "confirmed-veto") != NULL);
    CHECK(strstr(captured, "unresolved") != NULL);
    CHECK(strstr(captured, "test-veto") != NULL);
    CHECK(strstr(captured, "Ejection failed") == NULL);
}

static void test_portable_continuation_command_line(void) {
    Command command;
    wchar_t output[1024];
    LPWSTR *arguments;
    int argument_count;
    command_init(&command);
    command.card_mode = 1;
    command.quiet = 1;
    command.no_prompt = 1;
    command.format = OUTPUT_TSV;
    CHECK(portable_build_command_line(output, 1024,
        L"C:\\Temp Folder\\usb-eject.exe", 42, L"USB\\DEVICE",
        L"\\\\?\\Volume{abc}\\", L"E:\\", &command));
    CHECK(wcsstr(output, L"--internal-wait-pid 42") != NULL);
    CHECK(wcsstr(output, L" eject --mount") != NULL);
    CHECK(wcsstr(output, L"--card") != NULL);
    CHECK(wcsstr(output, L"--quiet") != NULL);
    CHECK(wcsstr(output, L"--no-prompt") != NULL);
    CHECK(wcsstr(output, L"--format tsv") != NULL);
    CHECK(wcsstr(output, L"USB\\DEVICE") != NULL);
    arguments = CommandLineToArgvW(output, &argument_count);
    CHECK(arguments != NULL);
    CHECK(argument_count >= 8);
    CHECK(wcscmp(arguments[3], L"USB\\DEVICE") == 0);
    CHECK(wcscmp(arguments[4], L"\\\\?\\Volume{abc}\\") == 0);
    CHECK(wcscmp(arguments[7], L"E:\\") == 0);
    LocalFree(arguments);
    CHECK(portable_is_temporary_copy_path(
        L"C:\\Temp\\usb-eject-0123456789abcdef0123456789abcdef\\usb-eject.exe",
        L"C:\\Temp\\"));
    CHECK(!portable_is_temporary_copy_path(
        L"C:\\Tools\\usb-eject.exe", L"C:\\Temp\\"));
    command_dispose(&command);
}

static void test_eject_veto_status_mapping(void) {
    CHECK(eject_status_from_veto(CR_SUCCESS, PNP_VetoTypeUnknown) == APP_OK);
    CHECK(eject_status_from_veto(1, PNP_VetoOutstandingOpen) == APP_BUSY);
    CHECK(eject_status_from_veto(1, PNP_VetoWindowsApp) == APP_BUSY);
    CHECK(eject_status_from_veto(1, PNP_VetoInsufficientRights) == APP_ACCESS_DENIED);
    CHECK(eject_status_from_veto(1, PNP_VetoNonDisableable) == APP_UNSUPPORTED);
}

static void test_diagnostic_completeness_precedence(void) {
    CHECK(diagnostic_completeness_from_issues(0) == DIAGNOSTIC_COMPLETE);
    CHECK(diagnostic_completeness_from_issues(DIAG_ISSUE_ACCESS) ==
        DIAGNOSTIC_PARTIAL_ACCESS);
    CHECK(diagnostic_completeness_from_issues(
        DIAG_ISSUE_ACCESS | DIAG_ISSUE_TIMEOUT) == DIAGNOSTIC_PARTIAL_TIMEOUT);
    CHECK(diagnostic_completeness_from_issues(
        DIAG_ISSUE_ACCESS | DIAG_ISSUE_CHANGED) == DIAGNOSTIC_CHANGED_DURING_SCAN);
}

static void test_kill_authorization_is_explicit_and_pid_scoped(void) {
    Command command;
    DWORD pids[] = { 42, 8420 };
    command_init(&command);
    command.authorized_pids = pids;
    command.authorized_pid_count = 2;
    CHECK(action_pid_authorized(&command, 42));
    CHECK(action_pid_authorized(&command, 8420));
    CHECK(!action_pid_authorized(&command, 41));
    command.authorized_pids = NULL;
    command.authorized_pid_count = 0;
    command_dispose(&command);
}

int main(void) {
    test_cli_help_and_missing_values();
    test_case_insensitive_matching();
    test_supported_patterns();
    test_path_boundaries();
    test_tsv_escaping();
    test_storage_descriptor_strings_are_bounds_checked();
    test_cli_happy_paths();
    test_cli_conventional_aliases_and_letter_validation();
    test_cli_rejects_unsafe_or_ambiguous_input();
    test_target_resolution_collapses_sibling_volumes();
    test_inventory_card_reader_classification_and_sorting();
    test_diagnostic_classification_names();
    test_diagnostic_finding_sorting();
    test_diagnostic_tsv_contains_veto_and_unresolved_rows();
    test_portable_continuation_command_line();
    test_eject_veto_status_mapping();
    test_diagnostic_completeness_precedence();
    test_kill_authorization_is_explicit_and_pid_scoped();

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }

    printf("all tests passed\n");
    return 0;
}
