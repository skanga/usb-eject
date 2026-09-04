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
        L"USB Disk Ejector CLI 0.1.0\r\n"
        L"\r\n"
        L"Usage:\r\n"
        L"  usb-eject.exe list [--format text|tsv]\r\n"
        L"  usb-eject.exe diagnose <target> [--format text|tsv]\r\n"
        L"  usb-eject.exe eject <target> [--card] [--quiet]\r\n"
        L"  usb-eject.exe eject <target> --kill-blocker <pid> [--yes]\r\n"
        L"  usb-eject.exe eject --letter <letter>\r\n"
        L"  usb-eject.exe eject --mount <path>\r\n"
        L"  usb-eject.exe eject --label <label-or-pattern>\r\n"
        L"  usb-eject.exe eject --name <name-or-pattern>\r\n"
        L"  usb-eject.exe eject --this\r\n"
        L"\r\n"
        L"Name and label patterns may use '*' at the beginning or end.\r\n"
        L"No device is ejected when a selector matches multiple devices.\r\n"
        L"--kill-blocker is PID-scoped, revalidates the blocker, and warns before\r\n"
        L"forced termination. --yes is valid only with an explicit blocker PID.\r\n"
        L"\r\n"
        L"Blocker diagnostics inspect system handles. Elevated mode improves\r\n"
        L"coverage, but protected processes and kernel drivers may remain opaque.\r\n");
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

    if (!output_write(0,
        L"mount_point\tlabel\tvendor\tproduct\trevision\tbus_type\t"
        L"card_reader\tmedia_present\tdevice_instance\r\n")) return 0;

    for (volume_index = 0; volume_index < inventory->count; volume_index++) {
        volume = &inventory->volumes[volume_index];
        media = volume->media_present < 0 ? L"" :
            (volume->media_present ? L"true" : L"false");
        for (mount_index = 0; mount_index < volume->mount_count; mount_index++) {
            if (!output_tsv_field(0, volume->mount_points[mount_index]) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->label) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->vendor) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->product) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->revision) ||
                !output_write(0, L"\t") || !output_tsv_field(0, inventory_bus_name(volume->bus_type)) ||
                !output_write(0, L"\tfalse\t") || !output_tsv_field(0, media) ||
                !output_write(0, L"\t") || !output_tsv_field(0, volume->instance_id) ||
                !output_write(0, L"\r\n")) return 0;
        }
    }
    return 1;
}

int output_inventory(const DeviceInventory *inventory, OutputFormat format) {
    size_t volume_index;
    size_t mount_index;
    const VolumeInfo *volume;

    if (format == OUTPUT_TSV) return output_inventory_tsv(inventory);
    if (inventory->count == 0) return output_write(0, L"No supported removable volumes found.\r\n");

    for (volume_index = 0; volume_index < inventory->count; volume_index++) {
        volume = &inventory->volumes[volume_index];
        for (mount_index = 0; mount_index < volume->mount_count; mount_index++) {
            if (!output_printf(0, L"%ls  %ls\r\n",
                    volume->mount_points[mount_index], safe(volume->label)) ||
                !output_printf(0, L"  Device: %ls %ls %ls\r\n",
                    safe(volume->vendor), safe(volume->product), safe(volume->revision)) ||
                !output_printf(0, L"  Bus: %ls\r\n", inventory_bus_name(volume->bus_type)) ||
                !output_printf(0, L"  Instance: %ls\r\n", safe(volume->instance_id))) return 0;
        }
    }
    return 1;
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

int output_diagnostic(
    const DiagnosticReport *report,
    const EjectResult *eject_result,
    OutputFormat format,
    int error_stream)
{
    size_t index;
    const BlockerFinding *finding;
    wchar_t *services;

    if (format == OUTPUT_TSV) {
        if (!output_write(error_stream,
                L"classification\tpid\tprocess_image\tprocess_user\tservices\t"
                L"object_type\tdos_path\tnt_path\thandle\twin32_error\t"
                L"veto_type\tveto_name\tcompleteness\r\n")) return 0;
        for (index = 0; index < report->finding_count; index++) {
            finding = &report->findings[index];
            services = joined_services(finding);
            if (services == NULL) return 0;
            if (!output_write(error_stream, L"blocking-candidate\t") ||
                !output_printf(error_stream, L"%lu\t", (unsigned long)finding->pid) ||
                !output_tsv_field(error_stream, finding->process_image) ||
                !output_write(error_stream, L"\t") ||
                !output_tsv_field(error_stream, finding->process_user) ||
                !output_write(error_stream, L"\t") ||
                !output_tsv_field(error_stream, services) ||
                !output_write(error_stream, L"\tFile\t") ||
                !output_tsv_field(error_stream, finding->dos_path) || !output_write(error_stream, L"\t") ||
                !output_tsv_field(error_stream, finding->nt_path) ||
                !output_printf(error_stream, L"\t0x%016llX\t\t%ls\t",
                    (unsigned long long)finding->handle_value,
                    eject_result != NULL ? eject_veto_name(eject_result->veto_type) : L"") ||
                !output_tsv_field(error_stream, eject_result != NULL ? eject_result->veto_name : L"") ||
                !output_printf(error_stream, L"\t%ls\r\n",
                    diagnostic_completeness_name(report->completeness))) {
                free(services);
                return 0;
            }
            free(services);
        }
    } else {
        if (report->finding_count == 0) {
            if (!output_write(error_stream,
                L"No user-mode open file handle could be associated with the device.\r\n")) return 0;
        } else {
            if (!output_write(error_stream, L"Blocking candidates:\r\n")) return 0;
            for (index = 0; index < report->finding_count; index++) {
                finding = &report->findings[index];
                services = joined_services(finding);
                if (services == NULL) return 0;
                if (!output_printf(error_stream, L"  PID %lu  %ls\r\n",
                        (unsigned long)finding->pid, safe(finding->process_image)) ||
                    !output_printf(error_stream, L"    user     %ls (session %lu)\r\n",
                        safe(finding->process_user), (unsigned long)finding->session_id) ||
                    (finding->service_count != 0 &&
                        !output_printf(error_stream, L"    services %ls\r\n", services)) ||
                    !output_printf(error_stream, L"    file     %ls\r\n",
                        safe(finding->dos_path)) ||
                    !output_printf(error_stream, L"    native   %ls\r\n",
                        safe(finding->nt_path)) ||
                    !output_printf(error_stream, L"    handle   0x%016llX\r\n",
                        (unsigned long long)finding->handle_value)) {
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
            !output_printf(error_stream,
                L"%u handle(s) changed during the scan.\r\n",
                (unsigned)report->changed_handle_count)) return 0;
    }
    return 1;
}
