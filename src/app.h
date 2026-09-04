#ifndef USB_EJECT_APP_H
#define USB_EJECT_APP_H

#include <windows.h>

typedef enum {
    APP_OK = 0,
    APP_USAGE = 1,
    APP_NOT_FOUND = 2,
    APP_AMBIGUOUS = 3,
    APP_BUSY = 4,
    APP_ACCESS_DENIED = 5,
    APP_UNSUPPORTED = 6,
    APP_INTERNAL_ERROR = 7,
    APP_NO_MEDIA = 8,
    APP_DIAGNOSTIC_INCOMPLETE = 9,
    APP_BLOCKER_ACTION_FAILED = 10,
    APP_OUT_OF_MEMORY = 100
} AppStatus;

typedef struct {
    AppStatus status;
    DWORD win32_error;
    ULONG config_ret;
    LONG nt_status;
    const wchar_t *operation;
} AppError;

static void app_error_clear(AppError *error) {
    if (error != NULL) {
        error->status = APP_OK;
        error->win32_error = 0;
        error->config_ret = 0;
        error->nt_status = 0;
        error->operation = NULL;
    }
}

#endif
