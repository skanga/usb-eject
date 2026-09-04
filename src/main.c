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
#include "target.h"

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
        L"Close it gracefully, then FORCE TERMINATE it if necessary?\r\n"
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
        if (report->findings[index].pid == pid) return &report->findings[index];
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

    diagnostic_report_init(&fresh);
    diagnostic_scan(inventory, target, &fresh, &error);
    finding = find_pid_finding(&fresh, pid);
    if (finding == NULL) {
        output_printf(1,
            L"Refusing process action: PID %lu is not a current blocker for this device.\r\n",
            (unsigned long)pid);
        diagnostic_report_dispose(&fresh);
        return -1;
    }

    confirmed = authorized_on_command_line && command->assume_yes;
    if (!confirmed) {
        if (!interactive_console() || command->no_prompt) {
            output_printf(1,
                L"Process action requires interactive confirmation or "
                L"--kill-blocker %lu --yes.\r\n", (unsigned long)pid);
            diagnostic_report_dispose(&fresh);
            return -1;
        }
        confirmed = confirm_blocker_action(finding);
    }
    if (!confirmed) {
        output_write(1, L"Process action declined; nothing was terminated.\r\n");
        diagnostic_report_dispose(&fresh);
        return 0;
    }

    status = action_stop_blocker(finding, 1, &action_result);
    if (status != APP_OK) {
        output_printf(1, L"Could not safely stop PID %lu (Windows error %lu).\r\n",
            (unsigned long)pid, (unsigned long)action_result.win32_error);
        diagnostic_report_dispose(&fresh);
        return -1;
    }
    if (action_result.forced_termination_used) {
        output_printf(1, L"PID %lu was forcibly terminated.\r\n", (unsigned long)pid);
    } else if (action_result.graceful_close_attempted) {
        output_printf(1, L"PID %lu closed gracefully.\r\n", (unsigned long)pid);
    } else {
        output_printf(1, L"PID %lu exited before process action was needed.\r\n",
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

    if (!interactive_console() || command->no_prompt) {
        for (index = 0; index < initial_report->finding_count; index++) {
            for (prior = 0; prior < index; prior++) {
                if (initial_report->findings[prior].pid ==
                    initial_report->findings[index].pid) break;
            }
            if (prior == index) {
                output_printf(1,
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
        if (prior != index) continue;
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

static int run_inventory_command(const Command *command) {
    DeviceInventory inventory;
    ResolvedTarget target;
    AppError error;
    EjectResult eject_result;
    DiagnosticReport diagnostic_report;
    AppStatus status;
    int blocker_action;

    inventory_init(&inventory);
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
        output_printf(1, L"Target is ambiguous: %u physical devices matched; nothing was ejected.\r\n",
            (unsigned)target.distinct_match_count);
    } else if (status == APP_UNSUPPORTED) {
        output_write(1, L"The volume was found, but a safe removable parent could not be established.\r\n");
    } else if (status != APP_OK) {
        output_app_error(&error);
    }
    if (status != APP_OK) {
        inventory_dispose(&inventory);
        return status;
    }

    if (command->kind == COMMAND_DIAGNOSE) {
        if (command->format == OUTPUT_TEXT) output_target(&inventory, &target);
        diagnostic_report_init(&diagnostic_report);
        status = diagnostic_scan(&inventory, &target, &diagnostic_report, &error);
        output_diagnostic(&diagnostic_report, NULL, command->format, 0);
        diagnostic_report_dispose(&diagnostic_report);
        inventory_dispose(&inventory);
        return status;
    }

    if (!command->quiet) output_target(&inventory, &target);
    if (command->card_mode) {
        status = eject_card_media(&inventory.volumes[target.volume_index], &eject_result);
    } else {
        status = eject_parent_device(target.removal_devinst, &eject_result);
    }
    if (status == APP_OK) {
        if (!command->quiet) output_write(0, L"Safe-removal request succeeded.\r\n");
    } else {
        output_eject_error(&eject_result);
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
            output_diagnostic(&diagnostic_report, &eject_result, command->format, 1);
            blocker_action = handle_blocker_actions(command, &inventory, &target,
                &diagnostic_report);
            diagnostic_report_dispose(&diagnostic_report);
            if (blocker_action < 0) {
                inventory_dispose(&inventory);
                return APP_BLOCKER_ACTION_FAILED;
            }
            if (blocker_action > 0) {
                diagnostic_report_init(&diagnostic_report);
                diagnostic_scan(&inventory, &target, &diagnostic_report, &error);
                if (diagnostic_report.finding_count != 0 ||
                    diagnostic_report.completeness != DIAGNOSTIC_COMPLETE) {
                    output_write(1,
                        L"The device still has blockers or an incomplete scan; ejection was not retried.\r\n");
                    output_diagnostic(&diagnostic_report, &eject_result,
                        command->format, 1);
                    diagnostic_report_dispose(&diagnostic_report);
                    inventory_dispose(&inventory);
                    return APP_BUSY;
                }
                diagnostic_report_dispose(&diagnostic_report);
                output_write(1, L"No blockers remain; retrying safe removal once.\r\n");
                if (command->card_mode) {
                    status = eject_card_media(&inventory.volumes[target.volume_index],
                        &eject_result);
                } else {
                    status = eject_parent_device(target.removal_devinst, &eject_result);
                }
                if (status == APP_OK) output_write(0, L"Safe-removal retry succeeded.\r\n");
                else output_eject_error(&eject_result);
            }
        }
    }
    inventory_dispose(&inventory);
    return status == APP_OUT_OF_MEMORY ? APP_INTERNAL_ERROR : status;
}

int main(void) {
    LPWSTR *arguments;
    int argument_count;
    Command command;
    ParseError parse_error;
    int result;

    arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == NULL) {
        output_write(1, L"Unable to read the Unicode command line.\r\n");
        return APP_INTERNAL_ERROR;
    }

    command_init(&command);
    if (!cli_parse(argument_count, arguments, &command, &parse_error)) {
        output_printf(1, L"Invalid command line near argument %d: %ls.\r\n",
            parse_error.argument_index, parse_error.message);
        output_write(1, L"Run usb-eject.exe help for usage.\r\n");
        command_dispose(&command);
        LocalFree(arguments);
        return APP_USAGE;
    }
    LocalFree(arguments);

    if (command.kind == COMMAND_HELP) result = output_help() ? APP_OK : APP_INTERNAL_ERROR;
    else if (command.kind == COMMAND_VERSION) result = output_version() ? APP_OK : APP_INTERNAL_ERROR;
    else result = run_inventory_command(&command);

    command_dispose(&command);
    return result;
}
