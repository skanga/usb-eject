#ifndef USB_EJECT_PORTABLE_H
#define USB_EJECT_PORTABLE_H

#include <stddef.h>

#include "app.h"
#include "cli.h"
#include "inventory.h"

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
    const VolumeInfo *volume,
    AppError *error);
AppStatus portable_wait_for_process(DWORD pid, AppError *error);
void portable_schedule_cleanup(void);
int portable_is_temporary_copy(void);
int portable_is_temporary_copy_path(
    const wchar_t *executable,
    const wchar_t *temporary_root);

#endif
