#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "portable.h"
#include "output.h"
#include "text.h"

typedef BOOLEAN (WINAPI *RtlGenRandomFn)(PVOID, ULONG);

typedef struct {
    wchar_t *data;
    size_t capacity;
    size_t length;
    int ok;
} CommandBuilder;

static void builder_append_char(CommandBuilder *builder, wchar_t value) {
    if (!builder->ok) return;
    if (builder->length + 1 >= builder->capacity) {
        builder->ok = 0;
        return;
    }
    builder->data[builder->length++] = value;
    builder->data[builder->length] = L'\0';
}

static void builder_append(CommandBuilder *builder, const wchar_t *value) {
    while (*value != L'\0') builder_append_char(builder, *value++);
}

static void builder_append_quoted(CommandBuilder *builder, const wchar_t *value) {
    size_t slashes;
    size_t index;
    builder_append_char(builder, L'"');
    while (*value != L'\0') {
        slashes = 0;
        while (value[slashes] == L'\\') slashes++;
        if (value[slashes] == L'"') {
            for (index = 0; index < slashes; index++) {
                builder_append_char(builder, L'\\');
                builder_append_char(builder, L'\\');
            }
            builder_append_char(builder, L'\\');
            builder_append_char(builder, L'"');
            value += slashes + 1;
        } else if (value[slashes] == L'\0') {
            for (index = 0; index < slashes; index++) {
                builder_append_char(builder, L'\\');
                builder_append_char(builder, L'\\');
            }
            value += slashes;
        } else {
            for (index = 0; index < slashes; index++) {
                builder_append_char(builder, L'\\');
            }
            value += slashes;
            builder_append_char(builder, *value);
            value++;
        }
    }
    builder_append_char(builder, L'"');
}

static void builder_space_arg(CommandBuilder *builder, const wchar_t *value) {
    builder_append_char(builder, L' ');
    builder_append_quoted(builder, value);
}

int portable_build_command_line(
    wchar_t *buffer,
    size_t capacity,
    const wchar_t *executable,
    DWORD original_pid,
    const wchar_t *instance_id,
    const wchar_t *volume_guid,
    const wchar_t *mount,
    const Command *command)
{
    CommandBuilder builder;
    wchar_t number[32];
    size_t index;
    if (buffer == NULL || capacity == 0 || executable == NULL ||
        instance_id == NULL || instance_id[0] == L'\0' ||
        volume_guid == NULL || volume_guid[0] == L'\0' || mount == NULL ||
        command == NULL || original_pid == 0) return 0;
    memset(&builder, 0, sizeof(builder));
    builder.data = buffer;
    builder.capacity = capacity;
    builder.ok = 1;
    buffer[0] = L'\0';
    builder_append_quoted(&builder, executable);
    builder_append(&builder, L" --internal-wait-pid ");
    _snwprintf(number, 32, L"%lu", (unsigned long)original_pid);
    builder_append(&builder, number);
    builder_space_arg(&builder, instance_id);
    builder_space_arg(&builder, volume_guid);
    builder_append(&builder, L" eject --mount");
    builder_space_arg(&builder, mount);
    if (command->card_mode) builder_append(&builder, L" --card");
    if (command->quiet) builder_append(&builder, L" --quiet");
    if (command->no_prompt) builder_append(&builder, L" --no-prompt");
    if (command->verbose) builder_append(&builder, L" --verbose");
    builder_append(&builder, L" --scan-timeout ");
    _snwprintf(number, 32, L"%lu", (unsigned long)command->scan_timeout_ms);
    builder_append(&builder, number);
    if (command->result_file != NULL) {
        builder_append(&builder, L" --result-file");
        builder_space_arg(&builder, command->result_file);
    }
    for (index = 0; index < command->authorized_pid_count; index++) {
        builder_append(&builder, L" --kill-blocker ");
        _snwprintf(number, 32, L"%lu",
            (unsigned long)command->authorized_pids[index]);
        builder_append(&builder, number);
    }
    if (command->assume_yes) builder_append(&builder, L" --yes");
    if (command->format == OUTPUT_TSV) builder_append(&builder, L" --format tsv");
    return builder.ok;
}

static int random_hex(wchar_t output[33]) {
    HMODULE library;
    RtlGenRandomFn random_bytes;
    unsigned char bytes[16];
    static const wchar_t hex[] = L"0123456789abcdef";
    size_t index;
    library = LoadLibraryW(L"advapi32.dll");
    if (library == NULL) return 0;
    random_bytes = (RtlGenRandomFn)GetProcAddress(library, "SystemFunction036");
    if (random_bytes == NULL || !random_bytes(bytes, sizeof(bytes))) {
        FreeLibrary(library);
        return 0;
    }
    FreeLibrary(library);
    for (index = 0; index < sizeof(bytes); index++) {
        output[index * 2] = hex[bytes[index] >> 4];
        output[index * 2 + 1] = hex[bytes[index] & 15];
    }
    output[32] = L'\0';
    return 1;
}

int portable_is_temporary_copy_path(
    const wchar_t *executable,
    const wchar_t *temporary_root)
{
    size_t root_length;
    const wchar_t *relative;
    size_t index;
    static const wchar_t prefix[] = L"usb-eject-";
    static const wchar_t suffix[] = L"\\usb-eject.exe";
    if (executable == NULL || temporary_root == NULL) return 0;
    root_length = wcslen(temporary_root);
    if (root_length == 0 || _wcsnicmp(executable, temporary_root, root_length) != 0) return 0;
    relative = executable + root_length;
    if (temporary_root[root_length - 1] != L'\\' &&
        temporary_root[root_length - 1] != L'/') {
        if (*relative != L'\\' && *relative != L'/') return 0;
        relative++;
    }
    if (_wcsnicmp(relative, prefix, wcslen(prefix)) != 0) return 0;
    relative += wcslen(prefix);
    for (index = 0; index < 32; index++) {
        wchar_t value = relative[index];
        if (!((value >= L'0' && value <= L'9') ||
              (value >= L'a' && value <= L'f') ||
              (value >= L'A' && value <= L'F'))) return 0;
    }
    return _wcsicmp(relative + 32, suffix) == 0;
}

int portable_is_temporary_copy(void) {
    wchar_t executable[32768];
    wchar_t temporary_root[32768];
    if (GetModuleFileNameW(NULL, executable, 32768) == 0 ||
        GetTempPathW(32768, temporary_root) == 0) return 0;
    return portable_is_temporary_copy_path(executable, temporary_root);
}

static void set_portable_error(AppError *error, DWORD code, const wchar_t *operation) {
    if (error == NULL) return;
    app_error_clear(error);
    error->status = code == ERROR_ACCESS_DENIED ? APP_ACCESS_DENIED : APP_INTERNAL_ERROR;
    error->win32_error = code;
    error->operation = operation;
}

int portable_path_outside_target(const wchar_t *path, const DeviceInventory *inventory,
    const ResolvedTarget *target) {
    wchar_t mount[32768];
    wchar_t guid[MAX_PATH + 1];
    size_t index;
    if (!GetVolumePathNameW(path, mount, 32768) ||
        !GetVolumeNameForVolumeMountPointW(mount, guid, MAX_PATH + 1)) return 0;
    for (index = 0; index < inventory->count; index++) {
        if (target_volume_in_scope(inventory, target, index) &&
            text_iequals(guid, inventory->volumes[index].volume_guid)) return 0;
    }
    return 1;
}

int portable_finish_result(const wchar_t *path, int exit_code) {
    HANDLE file;
    char result[64];
    DWORD written;
    int length;
    int ok;
    if (path == NULL) return 0;
    file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    length = _snprintf(result, sizeof(result), "exit_code=%d\r\n", exit_code);
    ok = length > 0 && WriteFile(file, result, (DWORD)length, &written, NULL) &&
        written == (DWORD)length && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

AppStatus portable_launch(
    const Command *command,
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    AppError *error)
{
    wchar_t source[32768];
    wchar_t temp[32768];
    wchar_t random[33];
    wchar_t directory[32768];
    wchar_t destination[32768];
    wchar_t command_line[32768];
    DWORD length;
    unsigned attempt;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD code;
    wchar_t receipt[32768];
    HANDLE result_file;
    DWORD written;
    Command continuation_command;
    const VolumeInfo *volume;
    static const char pending[] = "usb-eject-result-v1\r\nstate=pending\r\n";
    app_error_clear(error);
    if (inventory == NULL || target == NULL || inventory->volumes == NULL ||
        target->volume_index >= inventory->count) {
        set_portable_error(error, ERROR_INVALID_PARAMETER, L"prepare --this target");
        return APP_UNSUPPORTED;
    }
    volume = &inventory->volumes[target->volume_index];
    if (command == NULL || volume == NULL || volume->mount_count == 0 ||
        volume->instance_id == NULL || volume->instance_id[0] == L'\0' ||
        volume->volume_guid == NULL || volume->volume_guid[0] == L'\0') {
        set_portable_error(error, ERROR_INVALID_PARAMETER, L"prepare --this continuation");
        return APP_UNSUPPORTED;
    }
    length = GetModuleFileNameW(NULL, source, 32768);
    if (length == 0 || length >= 32767) {
        set_portable_error(error, GetLastError(), L"locate running executable");
        return APP_INTERNAL_ERROR;
    }
    length = GetTempPathW(32768, temp);
    if (length == 0 || length >= 32760) {
        set_portable_error(error, GetLastError(), L"resolve temporary directory");
        return APP_INTERNAL_ERROR;
    }
    if (!portable_path_outside_target(temp, inventory, target)) {
        set_portable_error(error, ERROR_INVALID_PARAMETER, L"temporary directory must be outside the device being removed");
        return APP_UNSUPPORTED;
    }
    directory[0] = L'\0';
    for (attempt = 0; attempt < 8; attempt++) {
        if (!random_hex(random)) {
            set_portable_error(error, GetLastError(), L"generate temporary name");
            return APP_INTERNAL_ERROR;
        }
        if (_snwprintf(directory, 32768, L"%lsusb-eject-%ls", temp, random) < 0) {
            set_portable_error(error, ERROR_INSUFFICIENT_BUFFER, L"build temporary path");
            return APP_INTERNAL_ERROR;
        }
        if (CreateDirectoryW(directory, NULL)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            set_portable_error(error, GetLastError(), L"create temporary directory");
            return APP_INTERNAL_ERROR;
        }
    }
    if (attempt == 8) {
        set_portable_error(error, ERROR_ALREADY_EXISTS, L"create unique temporary directory");
        return APP_INTERNAL_ERROR;
    }
    if (_snwprintf(destination, 32768, L"%ls\\usb-eject.exe", directory) < 0 ||
        !CopyFileW(source, destination, TRUE)) {
        code = GetLastError();
        RemoveDirectoryW(directory);
        set_portable_error(error, code, L"copy executable for --this");
        return code == ERROR_ACCESS_DENIED ? APP_ACCESS_DENIED : APP_INTERNAL_ERROR;
    }
    if (command->result_file != NULL) {
        length = GetFullPathNameW(command->result_file, 32768, receipt, NULL);
        if (length == 0 || length >= 32768) receipt[0] = L'\0';
    } else if (_snwprintf(receipt, 32768, L"%lsusb-eject-%ls.result", temp, random) < 0) {
        receipt[0] = L'\0';
    }
    if (receipt[0] == L'\0' || !portable_path_outside_target(receipt, inventory, target)) {
        DeleteFileW(destination);
        RemoveDirectoryW(directory);
        set_portable_error(error, ERROR_INVALID_PARAMETER, L"result receipt must be outside the device being removed");
        return APP_UNSUPPORTED;
    }
    result_file = CreateFileW(receipt, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (result_file == INVALID_HANDLE_VALUE) {
        code = GetLastError();
        DeleteFileW(destination);
        RemoveDirectoryW(directory);
        set_portable_error(error, code, L"create new result receipt (choose a path that does not exist)");
        return APP_INTERNAL_ERROR;
    }
    if (!WriteFile(result_file, pending, sizeof(pending) - 1, &written, NULL) ||
        written != sizeof(pending) - 1 || !FlushFileBuffers(result_file)) {
        code = GetLastError();
        CloseHandle(result_file);
        DeleteFileW(destination);
        RemoveDirectoryW(directory);
        set_portable_error(error, code, L"write pending result receipt");
        return APP_INTERNAL_ERROR;
    }
    CloseHandle(result_file);
    continuation_command = *command;
    continuation_command.result_file = receipt;
    if (!portable_build_command_line(command_line, 32768, destination,
            GetCurrentProcessId(), volume->instance_id, volume->volume_guid,
            volume->mount_points[0], &continuation_command)) {
        portable_finish_result(receipt, APP_INTERNAL_ERROR);
        DeleteFileW(destination);
        RemoveDirectoryW(directory);
        set_portable_error(error, ERROR_INSUFFICIENT_BUFFER, L"build continuation command");
        return APP_INTERNAL_ERROR;
    }
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (!CreateProcessW(destination, command_line, NULL, NULL, TRUE,
            0, NULL, directory, &startup, &process)) {
        code = GetLastError();
        portable_finish_result(receipt, code == ERROR_ACCESS_DENIED ? APP_ACCESS_DENIED : APP_INTERNAL_ERROR);
        DeleteFileW(destination);
        RemoveDirectoryW(directory);
        set_portable_error(error, code, L"start --this continuation");
        return code == ERROR_ACCESS_DENIED ? APP_ACCESS_DENIED : APP_INTERNAL_ERROR;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    output_printf(command->format == OUTPUT_TSV ? 0 : 1,
        L"Ejection started (exit 11); removal is pending. Final result receipt: %ls\r\n"
        L"Wait for an exit_code line; only exit_code=0 means removal succeeded.\r\n", receipt);
    return APP_STARTED;
}

AppStatus portable_wait_for_process(DWORD pid, AppError *error) {
    HANDLE process;
    DWORD result;
    app_error_clear(error);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        set_portable_error(error, ERROR_INVALID_PARAMETER, L"validate continuation PID");
        return APP_USAGE;
    }
    process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == NULL) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return APP_OK;
        set_portable_error(error, GetLastError(), L"open original process");
        return error != NULL ? error->status : APP_INTERNAL_ERROR;
    }
    result = WaitForSingleObject(process, 10000);
    CloseHandle(process);
    if (result == WAIT_OBJECT_0) return APP_OK;
    set_portable_error(error, result == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError(),
        L"wait for original executable to exit");
    return APP_INTERNAL_ERROR;
}

void portable_schedule_cleanup(void) {
    wchar_t executable[32768];
    wchar_t directory[32768];
    wchar_t temp[32768];
    wchar_t random[33];
    wchar_t script[32768];
    wchar_t system_directory[MAX_PATH + 1];
    wchar_t command_processor[MAX_PATH + 16];
    wchar_t command_line[32768];
    static const char body[] =
        "@echo off\r\n"
        "for /L %%G in (1,1,20) do (\r\n"
        "  del /f /q \"%~1\" >nul 2>&1\r\n"
        "  if not exist \"%~1\" goto done\r\n"
        "  ping 127.0.0.1 -n 2 >nul\r\n"
        ")\r\n"
        ":done\r\n"
        "rmdir /q \"%~2\" >nul 2>&1\r\n"
        "del /f /q \"%~f0\" >nul 2>&1\r\n";
    HANDLE file;
    DWORD written;
    wchar_t *slash;
    CommandBuilder builder;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    script[0] = L'\0';
    if (GetModuleFileNameW(NULL, executable, 32768) == 0) return;
    wcscpy(directory, executable);
    slash = wcsrchr(directory, L'\\');
    if (slash == NULL) return;
    *slash = L'\0';
    if (GetTempPathW(32768, temp) == 0 || !random_hex(random)) goto delayed;
    if (_snwprintf(script, 32768, L"%lsusb-eject-clean-%ls.cmd", temp, random) < 0) goto delayed;
    file = CreateFileW(script, GENERIC_WRITE, 0, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) goto delayed;
    if (!WriteFile(file, body, sizeof(body) - 1, &written, NULL) ||
        written != sizeof(body) - 1) {
        CloseHandle(file);
        DeleteFileW(script);
        goto delayed;
    }
    CloseHandle(file);
    if (GetSystemDirectoryW(system_directory, MAX_PATH + 1) == 0) goto delayed;
    _snwprintf(command_processor, MAX_PATH + 16, L"%ls\\cmd.exe", system_directory);
    memset(&builder, 0, sizeof(builder));
    builder.data = command_line;
    builder.capacity = 32768;
    builder.ok = 1;
    command_line[0] = L'\0';
    builder_append_quoted(&builder, command_processor);
    builder_append(&builder, L" /d /q /c call");
    builder_space_arg(&builder, script);
    builder_space_arg(&builder, executable);
    builder_space_arg(&builder, directory);
    if (!builder.ok) goto delayed;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    if (CreateProcessW(command_processor, command_line, NULL, NULL, FALSE,
            CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return;
    }
delayed:
    if (script[0] != L'\0') DeleteFileW(script);
    MoveFileExW(executable, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    MoveFileExW(directory, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
}
