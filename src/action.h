#ifndef USB_EJECT_ACTION_H
#define USB_EJECT_ACTION_H

#include "app.h"
#include "cli.h"
#include "diagnose.h"

typedef struct {
    int graceful_close_attempted;
    int forced_termination_used;
    DWORD win32_error;
} BlockerActionResult;

int action_pid_authorized(const Command *command, DWORD pid);
AppStatus action_stop_blocker(
    const BlockerFinding *finding,
    int allow_forced_termination,
    BlockerActionResult *result);

#endif
