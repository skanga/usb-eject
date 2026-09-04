#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "output.h"
#include "text.h"

#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080
#endif

static HANDLE stream_handle(int error_stream) {
    return GetStdHandle(error_stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
}

int output_write(int error_stream, const wchar_t *text) {
    HANDLE handle;
    DWORD mode;
    DWORD written;
    size_t length;
    int bytes_needed;
    char *bytes;
    BOOL ok;

    handle = stream_handle(error_stream);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) return 0;
    length = wcslen(text);
    if (length > 0xffffffffU) return 0;
    if (GetConsoleMode(handle, &mode)) {
        return WriteConsoleW(handle, text, (DWORD)length, &written, NULL) != FALSE;
    }

    if (length > 0x7fffffffU) return 0;
    bytes_needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text, (int)length, NULL, 0, NULL, NULL);
    if (bytes_needed == 0 && length != 0) return 0;
    bytes = (char *)malloc(bytes_needed == 0 ? 1 : (size_t)bytes_needed);
    if (bytes == NULL) return 0;
    if (bytes_needed != 0 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text, (int)length, bytes, bytes_needed, NULL, NULL) == 0) {
        free(bytes);
        return 0;
    }
    ok = WriteFile(handle, bytes, (DWORD)bytes_needed, &written, NULL);
    free(bytes);
    return ok != FALSE && written == (DWORD)bytes_needed;
}

int output_printf(int error_stream, const wchar_t *format, ...) {
    va_list arguments;
    int needed;
    wchar_t *buffer;
    int result;

    va_start(arguments, format);
    needed = _vscwprintf(format, arguments);
    va_end(arguments);
    if (needed < 0) return 0;
    buffer = (wchar_t *)malloc(((size_t)needed + 1) * sizeof(wchar_t));
    if (buffer == NULL) return 0;
    va_start(arguments, format);
    result = _vsnwprintf(buffer, (size_t)needed + 1, format, arguments);
    va_end(arguments);
    if (result < 0) {
        free(buffer);
        return 0;
    }
    buffer[needed] = L'\0';
    result = output_write(error_stream, buffer);
    free(buffer);
    return result;
}

int output_help(void) {
    return output_write(0,
        L"usb-eject 0.1.0\r\n"
        L"\r\n"
        L"Usage:\r\n"
        L"  usb-eject.exe help | --help | -h | /?\r\n"
        L"  usb-eject.exe version | --version\r\n"
        L"  usb-eject.exe list [--format text|tsv]\r\n"
        L"  usb-eject.exe diagnose <target-or-selector> [--format text|tsv]\r\n"
        L"  usb-eject.exe eject <target-or-selector> [options]\r\n"
        L"\r\n"
        L"Target selectors (use exactly one):\r\n"
        L"  <target>                 Drive letter or filesystem path\r\n"
        L"  --letter <A-Z>           Drive letter\r\n"
        L"  --mount <path>           Drive root or mounted-folder path\r\n"
        L"  --label <pattern>        Volume label\r\n"
        L"  --name <pattern>         Device name\r\n"
        L"  --this                   Device containing usb-eject.exe\r\n"
        L"\r\n"
        L"Eject options:\r\n"
        L"  --card                   Eject card media, not the reader\r\n"
        L"  --quiet                  Suppress successful eject output\r\n"
        L"  --no-prompt              Never ask to stop blocker processes\r\n"
        L"  --kill-blocker <pid>     Authorize one PID; may be repeated\r\n"
        L"  --yes                    Confirm explicitly authorized PIDs\r\n"
        L"  --format text|tsv        Select diagnostic format\r\n"
        L"\r\n"
        L"Examples:\r\n"
        L"  usb-eject.exe eject E:\r\n"
        L"  usb-eject.exe diagnose --label \"Work Backup\"\r\n"
        L"  usb-eject.exe eject --mount \"C:\\Mounts\\Camera\" --card\r\n"
        L"\r\n"
        L"Name and label patterns may use '*' at the beginning, end, or both.\r\n"
        L"No device is ejected when a selector matches multiple devices.\r\n"
        L"--kill-blocker is PID-scoped, revalidates the blocker, and warns before\r\n"
        L"forced termination. --yes is valid only with an explicit blocker PID.\r\n"
        L"\r\n"
        L"Blocker diagnostics inspect system handles. Elevated mode improves\r\n"
        L"coverage, but protected processes and kernel drivers may remain opaque.\r\n"
        L"Exit codes: 0 success, 1 usage, 2 not found, 3 ambiguous, 4 busy,\r\n"
        L"5 access denied, 6 unsupported, 7 internal, 8 no media,\r\n"
        L"9 incomplete diagnostic, 10 blocker action failed.\r\n");
}

int output_command_help(CommandKind command) {
    if (command == COMMAND_LIST) {
        return output_write(0,
            L"Usage: usb-eject.exe list [--format text|tsv]\r\n"
            L"Lists mounted, capability-checked USB/IEEE 1394 removable volumes.\r\n"
            L"Text includes device, bus, card-reader, media, and instance details.\r\n");
    }
    if (command == COMMAND_DIAGNOSE) {
        return output_write(0,
            L"Usage: usb-eject.exe diagnose <target-or-selector> [--format text|tsv]\r\n"
            L"Inspects open handles and processes without requesting removal.\r\n"
            L"Selectors: --letter, --mount, --label, --name, or --this.\r\n"
            L"Elevation improves coverage but does not guarantee a complete scan.\r\n");
    }
    if (command == COMMAND_EJECT) {
        return output_write(0,
            L"Usage: usb-eject.exe eject <target-or-selector> [options]\r\n"
            L"Selectors: --letter, --mount, --label, --name, or --this.\r\n"
            L"Options: --card, --quiet, --no-prompt, --format text|tsv,\r\n"
            L"         --kill-blocker <pid> (repeatable), and --yes.\r\n"
            L"Forced termination requires explicit PID authorization and confirmation.\r\n");
    }
    return output_help();
}

int output_version(void) {
    return output_write(0, L"usb-eject 0.1.0\r\n");
}

static const wchar_t *safe(const wchar_t *value) {
    return value != NULL ? value : L"";
}

static wchar_t *joined_services(const BlockerFinding *finding) {
    size_t total;
    size_t index;
    wchar_t *result;
    size_t length;

    total = 1;
    for (index = 0; index < finding->service_count; index++) {
        length = wcslen(finding->services[index]);
        if (total > (size_t)-1 - length - 2) return NULL;
        total += length + (index == 0 ? 0 : 2);
    }
    result = (wchar_t *)calloc(total, sizeof(wchar_t));
    if (result == NULL) return NULL;
    for (index = 0; index < finding->service_count; index++) {
        if (index != 0) wcscat(result, L", ");
        wcscat(result, finding->services[index]);
    }
    return result;
}

static int output_tsv_field(int error_stream, const wchar_t *value) {
    size_t needed;
    wchar_t *escaped;
    int result;

    needed = text_tsv_escape(safe(value), NULL, 0);
    escaped = (wchar_t *)malloc((needed + 1) * sizeof(wchar_t));
    if (escaped == NULL) return 0;
    text_tsv_escape(safe(value), escaped, needed + 1);
    result = output_write(error_stream, escaped);
    free(escaped);
    return result;
}

static int output_inventory_tsv(const DeviceInventory *inventory) {
    size_t volume_index;
    size_t mount_index;
    const VolumeInfo *volume;
    const wchar_t *media;
    const wchar_t *card;

    if (!output_write(0,
        L"mount_point\tlabel\tvendor\tproduct\trevision\tbus_type\t"
        L"card_reader\tmedia_present\tdevice_instance\r\n")) return 0;

    for (volume_index = 0; volume_index < inventory->count; volume_index++) {
        volume = &inventory->volumes[volume_index];
        if (volume->removal_devinst == 0) continue;
        media = volume->media_present < 0 ? L"" :
            (volume->media_present ? L"true" : L"false");
        card = volume->card_reader < 0 ? L"" :
            (volume->card_reader ? L"true" : L"false");
        for (mount_index = 0; mount_index < volume->mount_count; mount_index++) {
            if (!output_tsv_field(0, volume->mount_points[mount_index]) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->label) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->vendor) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->product) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->revision) ||
                !output_write(0, L"\t") || !output_tsv_field(0, inventory_bus_name(volume->bus_type)) ||
                !output_write(0, L"\t") || !output_tsv_field(0, card) ||
                !output_write(0, L"\t") || !output_tsv_field(0, media) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->instance_id) ||
                !output_write(0, L"\r\n")) return 0;
        }
    }
    return 1;
}

int output_inventory(const DeviceInventory *inventory, OutputFormat format) {
    size_t volume_index;
    size_t mount_index;
    size_t supported_count;
    const VolumeInfo *volume;

    if (format == OUTPUT_TSV) return output_inventory_tsv(inventory);

    supported_count = 0;
    for (volume_index = 0; volume_index < inventory->count; volume_index++) {
        if (inventory->volumes[volume_index].removal_devinst != 0) supported_count++;
    }
    if (supported_count == 0) return output_write(0, L"No supported removable volumes found.\r\n");

    for (volume_index = 0; volume_index < inventory->count; volume_index++) {
        volume = &inventory->volumes[volume_index];
        if (volume->removal_devinst == 0) continue;
        for (mount_index = 0; mount_index < volume->mount_count; mount_index++) {
            if (!output_printf(0, L"%ls  %ls\r\n",
                    volume->mount_points[mount_index], safe(volume->label)) ||
                !output_printf(0, L"  Device: %ls %ls %ls\r\n",
                    safe(volume->vendor), safe(volume->product), safe(volume->revision)) ||
                !output_printf(0, L"  Bus: %ls\r\n", inventory_bus_name(volume->bus_type)) ||
                !output_printf(0, L"  Card reader: %ls\r\n",
                    volume->card_reader < 0 ? L"unknown" :
                    (volume->card_reader ? L"yes" : L"no")) ||
                !output_printf(0, L"  Media present: %ls\r\n",
                    volume->media_present < 0 ? L"unknown" :
                    (volume->media_present ? L"yes" : L"no")) ||
                !output_printf(0, L"  Instance: %ls\r\n", safe(volume->instance_id))) return 0;
        }
    }
    return 1;
}

int output_ambiguous(const DeviceInventory *inventory, const ResolvedTarget *target) {
    size_t index;
    const VolumeInfo *volume;
    const wchar_t *mount;
    if (!output_printf(1,
            L"Target is ambiguous: %u physical devices matched; nothing was ejected.\r\n",
            (unsigned)target->distinct_match_count) ||
        !output_write(1, L"Matches:\r\n")) return 0;
    for (index = 0; index < target->distinct_match_count; index++) {
        volume = &inventory->volumes[target->distinct_volume_indexes[index]];
        mount = volume->mount_count != 0 ? volume->mount_points[0] : volume->volume_guid;
        if (!output_printf(1, L"  %ls  %ls %ls  [%ls]\r\n",
                safe(mount), safe(volume->vendor), safe(volume->product),
                safe(volume->instance_id))) return 0;
    }
    return output_write(1,
        L"Use --letter or --mount with one of the mount points above.\r\n");
}

int output_target(const DeviceInventory *inventory, const ResolvedTarget *target) {
    const VolumeInfo *volume;
    const wchar_t *mount;

    volume = &inventory->volumes[target->volume_index];
    mount = volume->mount_count != 0 ? volume->mount_points[0] : volume->volume_guid;
    if (!output_printf(0, L"Target: %ls (%ls %ls)\r\n",
            safe(mount), safe(volume->vendor), safe(volume->product))) return 0;
    if (target->sibling_volume_count > 1 &&
        !output_printf(0, L"The physical device contains %u mounted volumes; all will be removed.\r\n",
            (unsigned)target->sibling_volume_count)) return 0;
    return 1;
}

static void output_win32_message(DWORD code) {
    wchar_t *message;
    DWORD length;

    message = NULL;
    length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, code, 0, (LPWSTR)&message, 0, NULL);
    if (length != 0 && message != NULL) {
        output_printf(1, L"Windows error %lu: %ls", (unsigned long)code, message);
        if (length < 2 || message[length - 1] != L'\n') output_write(1, L"\r\n");
        LocalFree(message);
    } else {
        output_printf(1, L"Windows error %lu\r\n", (unsigned long)code);
    }
}

int output_app_error(const AppError *error) {
    if (error == NULL) return output_write(1, L"Internal error.\r\n");
    if (error->operation == NULL || error->operation[0] == L'\0') {
        if (error->status == APP_USAGE) return output_write(1, L"Invalid target value.\r\n");
        if (error->status == APP_OUT_OF_MEMORY) return output_write(1, L"Out of memory.\r\n");
        return output_write(1, L"The operation failed without additional Windows error information.\r\n");
    }
    output_printf(1, L"Error during %ls.\r\n", safe(error->operation));
    if (error->win32_error != 0) output_win32_message(error->win32_error);
    return 1;
}

int output_eject_error(const EjectResult *result) {
    output_printf(1, L"Ejection failed: %ls (CONFIGRET %lu).\r\n",
        eject_veto_name(result->veto_type), (unsigned long)result->config_ret);
    if (result->veto_name[0] != L'\0') {
        output_printf(1, L"Windows veto name: %ls\r\n", result->veto_name);
    }
    if (result->win32_error != 0) output_win32_message(result->win32_error);
    return 1;
}

static int output_diagnostic_tsv_row(
    int error_stream,
    DiagnosticClassification classification,
    const BlockerFinding *finding,
    const EjectResult *eject_result,
    DiagnosticCompleteness completeness)
{
    wchar_t *services;
    const wchar_t *object_type;
    DWORD win32_error;
    services = finding != NULL ? joined_services(finding) :
        (wchar_t *)calloc(1, sizeof(wchar_t));
    if (services == NULL) return 0;
    object_type = finding == NULL ? L"" :
        (classification == DIAGNOSTIC_BLOCKING_CANDIDATE ? L"File" :
         (classification == DIAGNOSTIC_PROCESS_ON_DEVICE ? L"Process" : L""));
    win32_error = finding != NULL ? finding->win32_error :
        (eject_result != NULL ? eject_result->win32_error : 0);
    if (!output_tsv_field(error_stream, diagnostic_classification_name(classification)) ||
        !output_write(error_stream, L"\t") ||
        (finding != NULL && finding->pid != 0 &&
            !output_printf(error_stream, L"%lu", (unsigned long)finding->pid)) ||
        !output_write(error_stream, L"\t") ||
        !output_tsv_field(error_stream, finding != NULL ? finding->process_image : L"") ||
        !output_write(error_stream, L"\t") ||
        !output_tsv_field(error_stream, finding != NULL ? finding->process_user : L"") ||
        !output_write(error_stream, L"\t") || !output_tsv_field(error_stream, services) ||
        !output_write(error_stream, L"\t") || !output_tsv_field(error_stream, object_type) ||
        !output_write(error_stream, L"\t") ||
        !output_tsv_field(error_stream, finding != NULL ? finding->dos_path : L"") ||
        !output_write(error_stream, L"\t") ||
        !output_tsv_field(error_stream, finding != NULL ? finding->nt_path : L"") ||
        !output_write(error_stream, L"\t") ||
        (finding != NULL && finding->handle_value != 0 &&
            !output_printf(error_stream, L"0x%016llX",
                (unsigned long long)finding->handle_value)) ||
        !output_write(error_stream, L"\t") ||
        (win32_error != 0 && !output_printf(error_stream, L"%lu", (unsigned long)win32_error)) ||
        !output_write(error_stream, L"\t") ||
        !output_tsv_field(error_stream, eject_result != NULL
            ? eject_veto_name(eject_result->veto_type) : L"") ||
        !output_write(error_stream, L"\t") ||
        !output_tsv_field(error_stream, eject_result != NULL ? eject_result->veto_name : L"") ||
        !output_write(error_stream, L"\t") ||
        !output_tsv_field(error_stream, diagnostic_completeness_name(completeness)) ||
        !output_write(error_stream, L"\r\n")) {
        free(services);
        return 0;
    }
    free(services);
    return 1;
}

static int output_diagnostic_core(
    const DiagnosticReport *report,
    const EjectResult *eject_result,
    OutputFormat format,
    int error_stream,
    int include_header)
{
    size_t index;
    const BlockerFinding *finding;
    BlockerFinding unresolved;
    wchar_t *services;
    if (format == OUTPUT_TSV) {
        if (include_header && !output_write(error_stream,
                L"classification\tpid\tprocess_image\tprocess_user\tservices\t"
                L"object_type\tdos_path\tnt_path\thandle\twin32_error\t"
                L"veto_type\tveto_name\tcompleteness\r\n")) return 0;
        if (eject_result != NULL && !output_diagnostic_tsv_row(error_stream,
                DIAGNOSTIC_CONFIRMED_VETO, NULL, eject_result,
                report->completeness)) return 0;
        for (index = 0; index < report->finding_count; index++) {
            finding = &report->findings[index];
            if (!output_diagnostic_tsv_row(error_stream, finding->classification,
                    finding, eject_result, report->completeness)) return 0;
        }
        if (report->completeness != DIAGNOSTIC_COMPLETE &&
            report->last_operation != NULL) {
            memset(&unresolved, 0, sizeof(unresolved));
            unresolved.classification = DIAGNOSTIC_UNRESOLVED_FINDING;
            unresolved.dos_path = (wchar_t *)report->last_operation;
            unresolved.win32_error = report->last_win32_error;
            if (!output_diagnostic_tsv_row(error_stream,
                    DIAGNOSTIC_UNRESOLVED_FINDING, &unresolved,
                    eject_result, report->completeness)) return 0;
        } else if (report->completeness != DIAGNOSTIC_COMPLETE &&
            !output_diagnostic_tsv_row(error_stream,
                DIAGNOSTIC_UNRESOLVED_FINDING, NULL,
                eject_result, report->completeness)) return 0;
        if (eject_result == NULL && report->finding_count == 0 &&
            report->completeness == DIAGNOSTIC_COMPLETE &&
            !output_diagnostic_tsv_row(error_stream, DIAGNOSTIC_SUMMARY,
                NULL, NULL, report->completeness)) return 0;
        return 1;
    }

    if (report->finding_count == 0) {
        if (!output_write(error_stream,
            L"No user-mode open file handle or running process could be associated with the device.\r\n")) return 0;
    } else {
        if (!output_write(error_stream, L"Diagnostic findings:\r\n")) return 0;
        for (index = 0; index < report->finding_count; index++) {
            finding = &report->findings[index];
            services = joined_services(finding);
            if (services == NULL) return 0;
            if (!output_printf(error_stream, L"  %ls  PID %lu  %ls\r\n",
                    diagnostic_classification_name(finding->classification),
                    (unsigned long)finding->pid, safe(finding->process_image)) ||
                !output_printf(error_stream, L"    user     %ls (session %lu)\r\n",
                    safe(finding->process_user), (unsigned long)finding->session_id) ||
                (finding->service_count != 0 &&
                    !output_printf(error_stream, L"    services %ls\r\n", services)) ||
                (safe(finding->dos_path)[0] != L'\0' &&
                    !output_printf(error_stream, L"    resource %ls\r\n", safe(finding->dos_path))) ||
                (safe(finding->nt_path)[0] != L'\0' &&
                    !output_printf(error_stream, L"    native   %ls\r\n", safe(finding->nt_path))) ||
                (finding->handle_value != 0 &&
                    !output_printf(error_stream, L"    handle   0x%016llX\r\n",
                        (unsigned long long)finding->handle_value))) {
                free(services);
                return 0;
            }
            free(services);
        }
    }
    if (!output_printf(error_stream, L"Inspected file handles: %u\r\n",
            (unsigned)report->inspected_file_handle_count) ||
        !output_printf(error_stream, L"Debug privilege: %ls\r\n",
            report->debug_privilege_enabled ? L"enabled" : L"not enabled") ||
        !output_printf(error_stream, L"Diagnostic completeness: %ls\r\n",
            diagnostic_completeness_name(report->completeness))) return 0;
    if (report->inaccessible_process_count != 0 &&
        !output_printf(error_stream,
            L"%u process handle(s) could not be inspected; rerun from an elevated terminal.\r\n",
            (unsigned)report->inaccessible_process_count)) return 0;
    if (report->changed_handle_count != 0 &&
        !output_printf(error_stream, L"%u handle(s) changed during the scan.\r\n",
            (unsigned)report->changed_handle_count)) return 0;
    if (report->last_operation != NULL &&
        !output_printf(error_stream, L"Inspection issue: %ls (Windows error %lu).\r\n",
            report->last_operation, (unsigned long)report->last_win32_error)) return 0;
    return 1;
}

int output_diagnostic(
    const DiagnosticReport *report,
    const EjectResult *eject_result,
    OutputFormat format,
    int error_stream)
{
    return output_diagnostic_core(report, eject_result, format, error_stream, 1);
}

int output_diagnostic_append(
    const DiagnosticReport *report,
    const EjectResult *eject_result,
    OutputFormat format,
    int error_stream)
{
    return output_diagnostic_core(report, eject_result, format, error_stream, 0);
}
