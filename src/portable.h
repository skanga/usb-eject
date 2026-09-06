#ifndef USB_EJECT_PORTABLE_H
#define USB_EJECT_PORTABLE_H

#include <stddef.h>

#include "app.h"
#include "cli.h"
#include "inventory.h"
#include "target.h"

int portable_build_command_line(
    wchar_t *buffer,
    size_t capacity,
    const wchar_t *executable,
    DWORD original_pid,
    const wchar_t *instance_id,
    const wchar_t *volume_guid,
    const wchar_t *mount,
    const Command *command);
AppStatus portable_launch(
    const Command *command,
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    AppError *error);
AppStatus portable_wait_for_process(DWORD pid, AppError *error);
int portable_finish_result(const wchar_t *path, int exit_code);
int portable_path_outside_target(const wchar_t *path, const DeviceInventory *inventory,
    const ResolvedTarget *target);
void portable_schedule_cleanup(void);
int portable_is_temporary_copy(void);
int portable_is_temporary_copy_path(
    const wchar_t *executable,
    const wchar_t *temporary_root);

#endif
