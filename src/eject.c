#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "eject.h"

#ifndef ERROR_NOT_READY
#define ERROR_NOT_READY 21
#endif
#ifndef FSCTL_LOCK_VOLUME
#define FSCTL_LOCK_VOLUME 0x00090018
#endif
#ifndef ERROR_NO_MEDIA_IN_DRIVE
#define ERROR_NO_MEDIA_IN_DRIVE 1112
#endif

static void eject_result_init(EjectResult *result) {
    memset(result, 0, sizeof(*result));
    result->status = APP_INTERNAL_ERROR;
    result->veto_type = PNP_VetoTypeUnknown;
}

AppStatus eject_status_from_veto(CONFIGRET config_ret, PNP_VETO_TYPE veto_type) {
    if (config_ret == CR_SUCCESS) return APP_OK;
    if (veto_type == PNP_VetoInsufficientRights) return APP_ACCESS_DENIED;
    if (veto_type == PNP_VetoLegacyDevice ||
        veto_type == PNP_VetoIllegalDeviceRequest ||
        veto_type == PNP_VetoNonDisableable) return APP_UNSUPPORTED;
    return APP_BUSY;
}

AppStatus eject_parent_device(DEVINST removal_devinst, EjectResult *result) {
    CONFIGRET config_ret;
    PNP_VETO_TYPE veto_type;
    unsigned attempt;

    eject_result_init(result);
    if (removal_devinst == 0) {
        result->status = APP_UNSUPPORTED;
        return result->status;
    }

    config_ret = 1;
    veto_type = PNP_VetoTypeUnknown;
    for (attempt = 0; attempt < 3; attempt++) {
        result->veto_name[0] = L'\0';
        veto_type = PNP_VetoTypeUnknown;
        config_ret = CM_Request_Device_EjectW(
            removal_devinst,
            &veto_type,
            result->veto_name,
            MAX_PATH,
            0);
        if (config_ret == CR_SUCCESS) break;
        if (veto_type != PNP_VetoPendingClose || attempt == 2) break;
        Sleep(attempt == 0 ? 250 : 750);
    }

    result->config_ret = config_ret;
    result->veto_type = veto_type;
    result->status = eject_status_from_veto(config_ret, veto_type);
    return result->status;
}

static wchar_t *volume_open_path(const wchar_t *volume_guid) {
    size_t length;
    wchar_t *path;

    length = wcslen(volume_guid);
    path = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (path == NULL) return NULL;
    memcpy(path, volume_guid, (length + 1) * sizeof(wchar_t));
    if (length > 0 && path[length - 1] == L'\\') path[length - 1] = L'\0';
    return path;
}

static AppStatus status_from_win32(DWORD error) {
    if (error == ERROR_ACCESS_DENIED) return APP_ACCESS_DENIED;
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) return APP_BUSY;
    if (error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE) return APP_NO_MEDIA;
    if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) return APP_UNSUPPORTED;
    return APP_INTERNAL_ERROR;
}

static AppStatus lock_card_volume(const VolumeInfo *volume, HANDLE *locked, EjectResult *result) {
    wchar_t *path;
    HANDLE handle;
    DWORD returned;
    DWORD error;

    eject_result_init(result);
    if (volume == NULL || volume->volume_guid == NULL) {
        result->status = APP_UNSUPPORTED;
        return result->status;
    }
    path = volume_open_path(volume->volume_guid);
    if (path == NULL) {
        result->status = APP_OUT_OF_MEMORY;
        result->win32_error = ERROR_NOT_ENOUGH_MEMORY;
        return result->status;
    }

    handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    free(path);
    if (handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        result->win32_error = error;
        result->status = status_from_win32(error);
        return result->status;
    }

    returned = 0;
    if (!DeviceIoControl(handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &returned, NULL)) {
        error = GetLastError();
        CloseHandle(handle);
        result->win32_error = error;
        /* Access denied after a successful read/write open commonly means open files. */
        result->status = error == ERROR_ACCESS_DENIED ? APP_BUSY : status_from_win32(error);
        return result->status;
    }
    if (!FlushFileBuffers(handle)) {
        error = GetLastError();
        CloseHandle(handle);
        result->win32_error = error;
        result->status = status_from_win32(error);
        return result->status;
    }

    returned = 0;
    if (!DeviceIoControl(handle, IOCTL_STORAGE_CHECK_VERIFY2,
            NULL, 0, NULL, 0, &returned, NULL)) {
        error = GetLastError();
        CloseHandle(handle);
        result->win32_error = error;
        result->status = status_from_win32(error);
        return result->status;
    }
    *locked = handle;
    return APP_OK;
}

AppStatus eject_card_target(const DeviceInventory *inventory,
    const ResolvedTarget *target, EjectResult *result) {
    HANDLE *handles;
    size_t index;
    DWORD returned;
    AppStatus status = APP_OK;
    eject_result_init(result);
    if (inventory == NULL || target == NULL || inventory->volumes == NULL ||
        target->volume_index >= inventory->count) {
        result->status = APP_UNSUPPORTED;
        return result->status;
    }
    handles = (HANDLE *)calloc(inventory->count, sizeof(HANDLE));
    if (handles == NULL) {
        result->status = APP_OUT_OF_MEMORY;
        result->win32_error = ERROR_NOT_ENOUGH_MEMORY;
        return result->status;
    }
    for (index = 0; index < inventory->count; index++) {
        if (!target_volume_in_scope(inventory, target, index)) continue;
        status = lock_card_volume(&inventory->volumes[index], &handles[index], result);
        if (status != APP_OK) break;
    }

    returned = 0;
    if (status == APP_OK && !DeviceIoControl(handles[target->volume_index], IOCTL_STORAGE_EJECT_MEDIA,
            NULL, 0, NULL, 0, &returned, NULL)) {
        result->win32_error = GetLastError();
        status = status_from_win32(result->win32_error);
    }
    for (index = 0; index < inventory->count; index++) {
        if (handles[index] != NULL) CloseHandle(handles[index]);
    }
    free(handles);
    result->status = status;
    return status;
}

AppStatus eject_card_media(const VolumeInfo *volume, EjectResult *result) {
    DeviceInventory inventory;
    ResolvedTarget target;
    memset(&inventory, 0, sizeof(inventory));
    memset(&target, 0, sizeof(target));
    inventory.volumes = (VolumeInfo *)volume;
    inventory.count = 1;
    target.media_scope = 1;
    return eject_card_target(&inventory, &target, result);
}

const wchar_t *eject_veto_name(PNP_VETO_TYPE veto_type) {
    switch (veto_type) {
        case PNP_VetoTypeUnknown: return L"PNP_VetoTypeUnknown";
        case PNP_VetoLegacyDevice: return L"PNP_VetoLegacyDevice";
        case PNP_VetoPendingClose: return L"PNP_VetoPendingClose";
        case PNP_VetoWindowsApp: return L"PNP_VetoWindowsApp";
        case PNP_VetoWindowsService: return L"PNP_VetoWindowsService";
        case PNP_VetoOutstandingOpen: return L"PNP_VetoOutstandingOpen";
        case PNP_VetoDevice: return L"PNP_VetoDevice";
        case PNP_VetoDriver: return L"PNP_VetoDriver";
        case PNP_VetoIllegalDeviceRequest: return L"PNP_VetoIllegalDeviceRequest";
        case PNP_VetoInsufficientPower: return L"PNP_VetoInsufficientPower";
        case PNP_VetoNonDisableable: return L"PNP_VetoNonDisableable";
        case PNP_VetoLegacyDriver: return L"PNP_VetoLegacyDriver";
        case PNP_VetoInsufficientRights: return L"PNP_VetoInsufficientRights";
        case PNP_VetoAlreadyRemoved: return L"PNP_VetoAlreadyRemoved";
        default: return L"PNP_VetoInvalid";
    }
}
