#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "action.h"
#include "app.h"
#include "cli.h"
#include "diagnose.h"
#include "eject.h"
#include "inventory.h"
#include "output.h"
#include "portable.h"
#include "target.h"
#include "text.h"

__declspec(dllimport) LPWSTR *WINAPI CommandLineToArgvW(LPCWSTR, int *);

static int interactive_console(void) {
    DWORD input_mode;
    DWORD error_mode;
    HANDLE input;
    HANDLE error_output;
    input = GetStdHandle(STD_INPUT_HANDLE);
    error_output = GetStdHandle(STD_ERROR_HANDLE);
    return input != INVALID_HANDLE_VALUE && error_output != INVALID_HANDLE_VALUE &&
        GetConsoleMode(input, &input_mode) && GetConsoleMode(error_output, &error_mode);
}

static int confirm_blocker_action(const BlockerFinding *finding) {
    wchar_t answer[16];
    DWORD read;
    output_printf(1,
        L"PID %lu (%ls) has a resource on the device.\r\n"
        L"Try to close it gracefully and then retry ejection? [y/N]: ",
        (unsigned long)finding->pid,
        finding->process_image != NULL ? finding->process_image : L"unknown");
    memset(answer, 0, sizeof(answer));
    if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), answer, 15, &read, NULL)) {
        output_write(1, L"\r\n");
        return 0;
    }
    return read != 0 && (answer[0] == L'y' || answer[0] == L'Y');
}

static int confirm_forced_termination(const BlockerFinding *finding) {
    wchar_t answer[16];
    DWORD read;
    output_printf(1,
        L"PID %lu (%ls) did not close gracefully. FORCE TERMINATE it?\r\n"
        L"Unsaved data may be lost. Type y to continue [y/N]: ",
        (unsigned long)finding->pid,
        finding->process_image != NULL ? finding->process_image : L"unknown");
    memset(answer, 0, sizeof(answer));
    if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), answer, 15, &read, NULL)) {
        output_write(1, L"\r\n");
        return 0;
    }
    return read != 0 && (answer[0] == L'y' || answer[0] == L'Y');
}

static const BlockerFinding *find_pid_finding(
    const DiagnosticReport *report,
    DWORD pid)
{
    size_t index;
    for (index = 0; index < report->finding_count; index++) {
        if (report->findings[index].pid == pid &&
            (report->findings[index].classification == DIAGNOSTIC_BLOCKING_CANDIDATE ||
             report->findings[index].classification == DIAGNOSTIC_PROCESS_ON_DEVICE)) {
            return &report->findings[index];
        }
    }
    return NULL;
}

static int act_on_one_pid(
    const Command *command,
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    DWORD pid,
    int authorized_on_command_line)
{
    DiagnosticReport fresh;
    const BlockerFinding *finding;
    BlockerActionResult action_result;
    AppError error;
    AppStatus status;
    int confirmed;
    int message_stream;

    message_stream = command->format == OUTPUT_TSV ? 0 : 1;

    diagnostic_report_init(&fresh);
    diagnostic_scan(inventory, target, &fresh, &error);
    finding = find_pid_finding(&fresh, pid);
    if (finding == NULL) {
        output_printf(message_stream,
            L"Refusing process action: PID %lu is not a current blocker for this device.\r\n",
            (unsigned long)pid);
        diagnostic_report_dispose(&fresh);
        return -1;
    }

    confirmed = authorized_on_command_line && command->assume_yes;
    if (!confirmed) {
        if (command->format == OUTPUT_TSV || !interactive_console() || command->no_prompt) {
            output_printf(message_stream,
                L"Process action requires interactive confirmation or "
                L"--kill-blocker %lu --yes.\r\n", (unsigned long)pid);
            diagnostic_report_dispose(&fresh);
            return -1;
        }
        confirmed = confirm_blocker_action(finding);
    }
    if (!confirmed) {
        output_write(message_stream, L"Process action declined; nothing was terminated.\r\n");
        diagnostic_report_dispose(&fresh);
        return 0;
    }

    status = action_stop_blocker(finding, 0, &action_result);
    if (status != APP_OK && action_result.win32_error == ERROR_CANCELLED) {
        if (authorized_on_command_line && command->assume_yes) {
            output_printf(message_stream,
                L"PID %lu did not close gracefully; --yes authorizes forced termination. "
                L"Unsaved data may be lost.\r\n", (unsigned long)pid);
            confirmed = 1;
        } else {
            confirmed = command->format == OUTPUT_TEXT && interactive_console() &&
                !command->no_prompt &&
                confirm_forced_termination(finding);
        }
        if (!confirmed) {
            output_write(message_stream,
                L"Forced termination declined; the process was left running.\r\n");
            diagnostic_report_dispose(&fresh);
            return 0;
        }
        status = action_stop_blocker(finding, 1, &action_result);
    }
    if (status != APP_OK) {
        output_printf(message_stream, L"Could not safely stop PID %lu (Windows error %lu).\r\n",
            (unsigned long)pid, (unsigned long)action_result.win32_error);
        diagnostic_report_dispose(&fresh);
        return -1;
    }
    if (action_result.forced_termination_used) {
        output_printf(message_stream, L"PID %lu was forcibly terminated.\r\n", (unsigned long)pid);
    } else if (action_result.graceful_close_attempted) {
        output_printf(message_stream, L"PID %lu closed gracefully.\r\n", (unsigned long)pid);
    } else {
        output_printf(message_stream, L"PID %lu exited before process action was needed.\r\n",
            (unsigned long)pid);
    }
    diagnostic_report_dispose(&fresh);
    return 1;
}

static int handle_blocker_actions(
    const Command *command,
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    const DiagnosticReport *initial_report)
{
    size_t index;
    size_t prior;
    int result;
    int acted;

    acted = 0;
    if (command->authorized_pid_count != 0) {
        for (index = 0; index < command->authorized_pid_count; index++) {
            result = act_on_one_pid(command, inventory, target,
                command->authorized_pids[index], 1);
            if (result < 0) return -1;
            if (result > 0) acted = 1;
        }
        return acted;
    }

    if (command->format == OUTPUT_TSV || !interactive_console() || command->no_prompt) {
        for (index = 0; index < initial_report->finding_count; index++) {
            for (prior = 0; prior < index; prior++) {
                if (initial_report->findings[prior].pid ==
                    initial_report->findings[index].pid) break;
            }
            if (prior == index &&
                (initial_report->findings[index].classification == DIAGNOSTIC_BLOCKING_CANDIDATE ||
                 initial_report->findings[index].classification == DIAGNOSTIC_PROCESS_ON_DEVICE)) {
                output_printf(command->format == OUTPUT_TSV ? 0 : 1,
                    L"To authorize this blocker, rerun with --kill-blocker %lu --yes.\r\n",
                    (unsigned long)initial_report->findings[index].pid);
            }
        }
        return 0;
    }

    for (index = 0; index < initial_report->finding_count; index++) {
        for (prior = 0; prior < index; prior++) {
            if (initial_report->findings[prior].pid ==
                initial_report->findings[index].pid) break;
        }
        if (prior != index ||
            (initial_report->findings[index].classification != DIAGNOSTIC_BLOCKING_CANDIDATE &&
             initial_report->findings[index].classification != DIAGNOSTIC_PROCESS_ON_DEVICE)) continue;
        result = act_on_one_pid(command, inventory, target,
            initial_report->findings[index].pid, 0);
        if (result < 0) return -1;
        if (result > 0) {
            acted = 1;
            break;
        }
    }
    return acted;
}

static int run_inventory_command(
    const Command *command,
    const wchar_t *expected_instance_id,
    const wchar_t *expected_volume_guid)
{
    DeviceInventory inventory;
    ResolvedTarget target;
    AppError error;
    EjectResult eject_result;
    DiagnosticReport diagnostic_report;
    AppStatus status;
    int blocker_action;
    int output_ok;
    int action_message_stream;

    inventory_init(&inventory);
    memset(&target, 0, sizeof(target));
    action_message_stream = command->format == OUTPUT_TSV ? 0 : 1;
    status = inventory_build(&inventory, &error);
    if (status != APP_OK) {
        output_app_error(&error);
        inventory_dispose(&inventory);
        return status == APP_OUT_OF_MEMORY ? APP_INTERNAL_ERROR : status;
    }

    if (command->kind == COMMAND_LIST) {
        status = output_inventory(&inventory, command->format)
            ? APP_OK : APP_INTERNAL_ERROR;
        inventory_dispose(&inventory);
        return status;
    }

    status = target_resolve(&inventory, &command->selector, &target, &error);
    if (status == APP_NOT_FOUND) {
        output_write(1, L"No supported removable device matched the target.\r\n");
    } else if (status == APP_AMBIGUOUS) {
        output_ambiguous(&inventory, &target);
    } else if (status == APP_UNSUPPORTED) {
        output_write(1, L"The volume was found, but a safe removable parent could not be established.\r\n");
    } else if (status != APP_OK) {
        output_app_error(&error);
    }
    if (status != APP_OK) {
        resolved_target_dispose(&target);
        inventory_dispose(&inventory);
        return status == APP_OUT_OF_MEMORY ? APP_INTERNAL_ERROR : status;
    }

    if (expected_instance_id != NULL &&
        (!text_iequals(inventory.volumes[target.volume_index].instance_id,
             expected_instance_id) ||
         !text_iequals(inventory.volumes[target.volume_index].volume_guid,
             expected_volume_guid))) {
        output_write(1,
            L"Refusing continuation: the removable device identity changed.\r\n");
        resolved_target_dispose(&target);
        inventory_dispose(&inventory);
        return APP_UNSUPPORTED;
    }

    if (command->kind == COMMAND_EJECT &&
        command->selector.kind == SELECTOR_THIS) {
        status = portable_launch(command, &inventory.volumes[target.volume_index], &error);
        if (status != APP_OK) {
            output_app_error(&error);
        } else if (!command->quiet) {
            if (!output_write(0,
                    L"Started a temporary continuation; it will eject the device after this process exits.\r\n")) {
                status = APP_INTERNAL_ERROR;
            }
        }
        resolved_target_dispose(&target);
        inventory_dispose(&inventory);
        return status;
    }

    if (command->kind == COMMAND_DIAGNOSE) {
        if (command->format == OUTPUT_TEXT && !output_target(&inventory, &target)) {
            resolved_target_dispose(&target);
            inventory_dispose(&inventory);
            return APP_INTERNAL_ERROR;
        }
        diagnostic_report_init(&diagnostic_report);
        status = diagnostic_scan(&inventory, &target, &diagnostic_report, &error);
        output_ok = output_diagnostic(&diagnostic_report, NULL, command->format, 0);
        diagnostic_report_dispose(&diagnostic_report);
        resolved_target_dispose(&target);
        inventory_dispose(&inventory);
        return output_ok ? status : APP_INTERNAL_ERROR;
    }

    if (!command->quiet && !output_target(&inventory, &target)) {
        resolved_target_dispose(&target);
        inventory_dispose(&inventory);
        return APP_INTERNAL_ERROR;
    }
    if (command->card_mode) {
        status = eject_card_media(&inventory.volumes[target.volume_index], &eject_result);
    } else {
        status = eject_parent_device(target.removal_devinst, &eject_result);
    }
    if (status == APP_OK) {
        if (!command->quiet && !output_write(0, L"Safe-removal request succeeded.\r\n")) {
            status = APP_INTERNAL_ERROR;
        }
    } else {
        if (command->format == OUTPUT_TEXT) output_eject_error(&eject_result);
        if (status == APP_BUSY) {
            diagnostic_report_init(&diagnostic_report);
            diagnostic_scan(&inventory, &target, &diagnostic_report, &error);
            if (eject_result.veto_type == PNP_VetoDriver ||
                eject_result.veto_type == PNP_VetoLegacyDriver ||
                eject_result.veto_type == PNP_VetoDevice) {
                diagnostic_report.issue_flags |= DIAG_ISSUE_KERNEL;
                diagnostic_report.completeness = diagnostic_completeness_from_issues(
                    diagnostic_report.issue_flags);
            }
            if (!output_diagnostic(&diagnostic_report, &eject_result, command->format, 1)) {
                diagnostic_report_dispose(&diagnostic_report);
                resolved_target_dispose(&target);
                inventory_dispose(&inventory);
                return APP_INTERNAL_ERROR;
            }
            blocker_action = handle_blocker_actions(command, &inventory, &target,
                &diagnostic_report);
            diagnostic_report_dispose(&diagnostic_report);
            if (blocker_action < 0) {
                resolved_target_dispose(&target);
                inventory_dispose(&inventory);
                return APP_BLOCKER_ACTION_FAILED;
            }
            if (blocker_action > 0) {
                diagnostic_report_init(&diagnostic_report);
                diagnostic_scan(&inventory, &target, &diagnostic_report, &error);
                if (diagnostic_report.finding_count != 0 ||
                    diagnostic_report.completeness != DIAGNOSTIC_COMPLETE) {
                    output_write(action_message_stream,
                        L"The device still has blockers or an incomplete scan; ejection was not retried.\r\n");
                    output_diagnostic_append(&diagnostic_report, &eject_result,
                        command->format, 1);
                    diagnostic_report_dispose(&diagnostic_report);
                    resolved_target_dispose(&target);
                    inventory_dispose(&inventory);
                    return APP_BUSY;
                }
                diagnostic_report_dispose(&diagnostic_report);
                output_write(action_message_stream,
                    L"No blockers remain; retrying safe removal once.\r\n");
                if (command->card_mode) {
                    status = eject_card_media(&inventory.volumes[target.volume_index],
                        &eject_result);
                } else {
                    status = eject_parent_device(target.removal_devinst, &eject_result);
                }
                if (status == APP_OK) {
                    if (!output_write(0, L"Safe-removal retry succeeded.\r\n")) {
                        status = APP_INTERNAL_ERROR;
                    }
                } else if (command->format == OUTPUT_TEXT) {
                    output_eject_error(&eject_result);
                } else {
                    diagnostic_report_init(&diagnostic_report);
                    diagnostic_report.completeness = DIAGNOSTIC_COMPLETE;
                    output_diagnostic_append(&diagnostic_report, &eject_result,
                        command->format, 1);
                    diagnostic_report_dispose(&diagnostic_report);
                }
            }
        } else if (command->format == OUTPUT_TSV) {
            diagnostic_report_init(&diagnostic_report);
            diagnostic_report.completeness = DIAGNOSTIC_COMPLETE;
            if (!output_diagnostic(&diagnostic_report, &eject_result,
                    command->format, 1)) status = APP_INTERNAL_ERROR;
            diagnostic_report_dispose(&diagnostic_report);
        }
    }
    resolved_target_dispose(&target);
    inventory_dispose(&inventory);
    return status == APP_OUT_OF_MEMORY ? APP_INTERNAL_ERROR : status;
}

int main(void) {
    LPWSTR *arguments;
    LPWSTR *raw_arguments;
    int argument_count;
    Command command;
    ParseError parse_error;
    AppError continuation_error;
    int result;
    int worker_exit;
    int continuation;
    DWORD original_pid;
    size_t digit_index;
    unsigned long long parsed_pid;
    wchar_t *expected_instance_id;
    wchar_t *expected_volume_guid;
    size_t expected_length;

    if (diagnostic_run_worker_if_requested(&worker_exit)) return worker_exit;

    arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == NULL) {
        output_write(1, L"Unable to read the Unicode command line.\r\n");
        return APP_INTERNAL_ERROR;
    }
    raw_arguments = arguments;
    continuation = 0;
    expected_instance_id = NULL;
    expected_volume_guid = NULL;
    if (argument_count >= 2 &&
        text_iequals(arguments[1], L"--internal-wait-pid")) {
        if (!portable_is_temporary_copy()) {
            output_write(1, L"Invalid internal continuation command.\r\n");
            LocalFree(raw_arguments);
            return APP_USAGE;
        }
        continuation = 1;
        if (argument_count < 7 || arguments[3][0] == L'\0' ||
            arguments[4][0] == L'\0') {
            output_write(1, L"Invalid internal continuation command.\r\n");
            portable_schedule_cleanup();
            LocalFree(raw_arguments);
            return APP_USAGE;
        }
        parsed_pid = 0;
        for (digit_index = 0; arguments[2][digit_index] != L'\0'; digit_index++) {
            if (arguments[2][digit_index] < L'0' || arguments[2][digit_index] > L'9') {
                parsed_pid = 0;
                break;
            }
            parsed_pid = parsed_pid * 10 +
                (unsigned)(arguments[2][digit_index] - L'0');
            if (parsed_pid > 0xffffffffULL) {
                parsed_pid = 0;
                break;
            }
        }
        if (parsed_pid == 0) {
            output_write(1, L"Invalid internal continuation PID.\r\n");
            portable_schedule_cleanup();
            LocalFree(raw_arguments);
            return APP_USAGE;
        }
        original_pid = (DWORD)parsed_pid;
        expected_length = wcslen(arguments[3]);
        expected_instance_id = (wchar_t *)malloc((expected_length + 1) * sizeof(wchar_t));
        expected_volume_guid = (wchar_t *)malloc((wcslen(arguments[4]) + 1) * sizeof(wchar_t));
        if (expected_instance_id == NULL || expected_volume_guid == NULL) {
            free(expected_instance_id);
            free(expected_volume_guid);
            portable_schedule_cleanup();
            LocalFree(raw_arguments);
            return APP_INTERNAL_ERROR;
        }
        wcscpy(expected_instance_id, arguments[3]);
        wcscpy(expected_volume_guid, arguments[4]);
        app_error_clear(&continuation_error);
        result = portable_wait_for_process(original_pid, &continuation_error);
        if (result != APP_OK) {
            output_app_error(&continuation_error);
            portable_schedule_cleanup();
            free(expected_instance_id);
            free(expected_volume_guid);
            LocalFree(raw_arguments);
            return result;
        }
        arguments += 4;
        argument_count -= 4;
    }

    command_init(&command);
    if (!cli_parse(argument_count, arguments, &command, &parse_error)) {
        output_printf(1, L"Invalid command line near argument %d: %ls.\r\n",
            parse_error.argument_index, parse_error.message);
        output_write(1, L"Run usb-eject.exe help for usage.\r\n");
        command_dispose(&command);
        LocalFree(raw_arguments);
        if (continuation) portable_schedule_cleanup();
        free(expected_instance_id);
        free(expected_volume_guid);
        return APP_USAGE;
    }
    if (continuation &&
        (command.kind != COMMAND_EJECT || command.selector.kind != SELECTOR_MOUNT)) {
        output_write(1, L"Invalid internal continuation operation.\r\n");
        command_dispose(&command);
        LocalFree(raw_arguments);
        portable_schedule_cleanup();
        free(expected_instance_id);
        free(expected_volume_guid);
        return APP_USAGE;
    }
    LocalFree(raw_arguments);

    if (command.kind == COMMAND_HELP) result =
        output_command_help(command.help_topic) ? APP_OK : APP_INTERNAL_ERROR;
    else if (command.kind == COMMAND_VERSION) result = output_version() ? APP_OK : APP_INTERNAL_ERROR;
    else result = run_inventory_command(&command,
        expected_instance_id, expected_volume_guid);

    command_dispose(&command);
    if (continuation) portable_schedule_cleanup();
    free(expected_instance_id);
    free(expected_volume_guid);
    return result;
}
