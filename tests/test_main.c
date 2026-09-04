#define UNICODE
#define _UNICODE

#include <stdio.h>
#include <wchar.h>

#include "../src/action.h"
#include "../src/cli.h"
#include "../src/diagnose.h"
#include "../src/eject.h"
#include "../src/inventory.h"
#include "../src/target.h"
#include "../src/text.h"

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

    selector.kind = SELECTOR_NAME;
    selector.value = L"Fast*";
    CHECK(target_resolve(&inventory, &selector, &resolved, &error) == APP_OK);
    CHECK(resolved.removal_devinst == 100);

    selector.kind = SELECTOR_LABEL;
    selector.value = L"Photos";
    CHECK(target_resolve(&inventory, &selector, &resolved, &error) == APP_AMBIGUOUS);
    CHECK(resolved.distinct_match_count == 2);
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
    test_case_insensitive_matching();
    test_supported_patterns();
    test_path_boundaries();
    test_tsv_escaping();
    test_storage_descriptor_strings_are_bounds_checked();
    test_cli_happy_paths();
    test_cli_rejects_unsafe_or_ambiguous_input();
    test_target_resolution_collapses_sibling_volumes();
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
