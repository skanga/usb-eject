#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "inventory.h"

const GUID USB_EJECT_GUID_DEVINTERFACE_DISK = {
    0x53f56307, 0xb6bf, 0x11d0,
    { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b }
};

static wchar_t *wide_duplicate(const wchar_t *value) {
    size_t length;
    wchar_t *copy;

    if (value == NULL) value = L"";
    length = wcslen(value);
    if (length > ((size_t)-1) / sizeof(wchar_t) - 1) return NULL;
    copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (copy != NULL) memcpy(copy, value, (length + 1) * sizeof(wchar_t));
    return copy;
}

static void volume_init(VolumeInfo *volume) {
    memset(volume, 0, sizeof(*volume));
    volume->media_present = -1;
}

static void volume_dispose(VolumeInfo *volume) {
    size_t index;
    free(volume->volume_guid);
    free(volume->native_path);
    free(volume->label);
    free(volume->vendor);
    free(volume->product);
    free(volume->revision);
    free(volume->instance_id);
    for (index = 0; index < volume->mount_count; index++) {
        free(volume->mount_points[index]);
    }
    free(volume->mount_points);
    volume_init(volume);
}

void inventory_init(DeviceInventory *inventory) {
    memset(inventory, 0, sizeof(*inventory));
}

void inventory_dispose(DeviceInventory *inventory) {
    size_t index;
    if (inventory == NULL) return;
    for (index = 0; index < inventory->count; index++) {
        volume_dispose(&inventory->volumes[index]);
    }
    free(inventory->volumes);
    inventory_init(inventory);
}

static void set_error(AppError *error, AppStatus status, DWORD code, const wchar_t *operation) {
    if (error != NULL) {
        error->status = status;
        error->win32_error = code;
        error->operation = operation;
    }
}

int inventory_copy_descriptor_string(
    const unsigned char *descriptor,
    size_t descriptor_size,
    DWORD offset,
    wchar_t *output,
    size_t output_capacity)
{
    size_t end;
    size_t first;
    size_t last;
    size_t length;
    size_t index;

    if (output == NULL || output_capacity == 0) return 0;
    output[0] = L'\0';
    if (descriptor == NULL || offset == 0 || (size_t)offset >= descriptor_size) return 0;

    end = (size_t)offset;
    while (end < descriptor_size && descriptor[end] != 0) end++;
    if (end == descriptor_size) return 0;

    first = (size_t)offset;
    while (first < end && descriptor[first] <= 0x20) first++;
    last = end;
    while (last > first && descriptor[last - 1] <= 0x20) last--;
    length = last - first;
    if (length + 1 > output_capacity) return 0;

    for (index = 0; index < length; index++) {
        output[index] = (wchar_t)descriptor[first + index];
    }
    output[length] = L'\0';
    return 1;
}

static int append_mount(VolumeInfo *volume, const wchar_t *mount) {
    wchar_t **new_values;
    wchar_t *copy;
    size_t new_count;

    if (volume->mount_count == (size_t)-1) return 0;
    new_count = volume->mount_count + 1;
    if (new_count > ((size_t)-1) / sizeof(wchar_t *)) return 0;
    copy = wide_duplicate(mount);
    if (copy == NULL) return 0;
    new_values = (wchar_t **)realloc(
        volume->mount_points, new_count * sizeof(wchar_t *));
    if (new_values == NULL) {
        free(copy);
        return 0;
    }
    volume->mount_points = new_values;
    volume->mount_points[volume->mount_count] = copy;
    volume->mount_count = new_count;
    return 1;
}

static int append_multisz_mounts(VolumeInfo *volume, const wchar_t *values, size_t characters) {
    size_t offset;
    size_t length;

    offset = 0;
    while (offset < characters && values[offset] != L'\0') {
        length = wcslen(values + offset);
        if (length >= characters - offset) return 0;
        if (!append_mount(volume, values + offset)) return 0;
        offset += length + 1;
    }
    return 1;
}

static int query_mounts(const wchar_t *volume_guid, VolumeInfo *volume, DWORD *error_code) {
    DWORD needed;
    wchar_t *buffer;
    BOOL result;

    needed = 0;
    SetLastError(ERROR_SUCCESS);
    result = GetVolumePathNamesForVolumeNameW(volume_guid, NULL, 0, &needed);
    if (result) return 1;
    if (GetLastError() != ERROR_MORE_DATA || needed < 2 ||
        needed > 1024U * 1024U) {
        *error_code = GetLastError();
        return 0;
    }
    buffer = (wchar_t *)calloc((size_t)needed, sizeof(wchar_t));
    if (buffer == NULL) {
        *error_code = ERROR_NOT_ENOUGH_MEMORY;
        return 0;
    }
    result = GetVolumePathNamesForVolumeNameW(volume_guid, buffer, needed, &needed);
    if (!result) {
        *error_code = GetLastError();
        free(buffer);
        return 0;
    }
    result = append_multisz_mounts(volume, buffer, needed) ? TRUE : FALSE;
    if (!result) *error_code = ERROR_NOT_ENOUGH_MEMORY;
    free(buffer);
    return result != FALSE;
}

static wchar_t *volume_open_path(const wchar_t *volume_guid) {
    wchar_t *path;
    size_t length;

    path = wide_duplicate(volume_guid);
    if (path == NULL) return NULL;
    length = wcslen(path);
    if (length > 0 && path[length - 1] == L'\\') path[length - 1] = L'\0';
    return path;
}

static int query_storage(HANDLE handle, VolumeInfo *volume, DWORD *error_code) {
    STORAGE_PROPERTY_QUERY query;
    STORAGE_DESCRIPTOR_HEADER header;
    STORAGE_DEVICE_DESCRIPTOR *descriptor;
    STORAGE_DEVICE_NUMBER number;
    DWORD returned;
    wchar_t field[256];
    BOOL ok;

    memset(&number, 0, sizeof(number));
    returned = 0;
    if (!DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            NULL, 0, &number, sizeof(number), &returned, NULL) ||
        returned < sizeof(number)) {
        *error_code = GetLastError();
        return 0;
    }

    memset(&query, 0, sizeof(query));
    query.PropertyId = STORAGE_DEVICE_PROPERTY;
    query.QueryType = PROPERTY_STANDARD_QUERY;
    memset(&header, 0, sizeof(header));
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query), &header, sizeof(header), &returned, NULL) ||
        returned < sizeof(header) || header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR) ||
        header.Size > 1024U * 1024U) {
        *error_code = GetLastError();
        return 0;
    }

    descriptor = (STORAGE_DEVICE_DESCRIPTOR *)calloc(1, header.Size);
    if (descriptor == NULL) {
        *error_code = ERROR_NOT_ENOUGH_MEMORY;
        return 0;
    }
    descriptor->Size = header.Size;
    ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), descriptor, header.Size, &returned, NULL);
    if (!ok || returned < sizeof(STORAGE_DEVICE_DESCRIPTOR) || returned > header.Size) {
        *error_code = GetLastError();
        free(descriptor);
        return 0;
    }

    volume->device_type = number.DeviceType;
    volume->device_number = number.DeviceNumber;
    volume->partition_number = number.PartitionNumber;
    volume->bus_type = descriptor->BusType;
    volume->removable_media = descriptor->RemovableMedia ? 1 : 0;

    if (descriptor->VendorIdOffset != 0) {
        if (!inventory_copy_descriptor_string((const unsigned char *)descriptor, returned,
                descriptor->VendorIdOffset, field, 256)) {
            free(descriptor);
            *error_code = ERROR_INVALID_DATA;
            return 0;
        }
        volume->vendor = wide_duplicate(field);
        if (volume->vendor == NULL) {
            free(descriptor);
            *error_code = ERROR_NOT_ENOUGH_MEMORY;
            return 0;
        }
    }
    if (descriptor->ProductIdOffset != 0) {
        if (!inventory_copy_descriptor_string((const unsigned char *)descriptor, returned,
                descriptor->ProductIdOffset, field, 256)) {
            free(descriptor);
            *error_code = ERROR_INVALID_DATA;
            return 0;
        }
        volume->product = wide_duplicate(field);
        if (volume->product == NULL) {
            free(descriptor);
            *error_code = ERROR_NOT_ENOUGH_MEMORY;
            return 0;
        }
    }
    if (descriptor->ProductRevisionOffset != 0) {
        if (!inventory_copy_descriptor_string((const unsigned char *)descriptor, returned,
                descriptor->ProductRevisionOffset, field, 256)) {
            free(descriptor);
            *error_code = ERROR_INVALID_DATA;
            return 0;
        }
        volume->revision = wide_duplicate(field);
        if (volume->revision == NULL) {
            free(descriptor);
            *error_code = ERROR_NOT_ENOUGH_MEMORY;
            return 0;
        }
    }
    free(descriptor);
    return 1;
}

static wchar_t *query_native_path(const wchar_t *volume_guid) {
    size_t length;
    wchar_t *device_name;
    wchar_t *buffer;
    DWORD capacity;
    DWORD result;

    length = wcslen(volume_guid);
    if (length < 6 || length > 32768) return NULL;
    device_name = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (device_name == NULL) return NULL;
    memcpy(device_name, volume_guid + 4, (length - 4 + 1) * sizeof(wchar_t));
    length = wcslen(device_name);
    if (length > 0 && device_name[length - 1] == L'\\') device_name[length - 1] = L'\0';

    capacity = 512;
    while (capacity <= 1024U * 1024U) {
        buffer = (wchar_t *)malloc((size_t)capacity * sizeof(wchar_t));
        if (buffer == NULL) break;
        result = QueryDosDeviceW(device_name, buffer, capacity);
        if (result != 0) {
            free(device_name);
            return buffer;
        }
        free(buffer);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        capacity *= 2;
    }
    free(device_name);
    return NULL;
}

static int query_label(const wchar_t *volume_guid, VolumeInfo *volume) {
    wchar_t label[MAX_PATH + 1];
    DWORD serial;
    DWORD maximum_component;
    DWORD flags;

    label[0] = L'\0';
    if (!GetVolumeInformationW(volume_guid, label, MAX_PATH + 1,
            &serial, &maximum_component, &flags, NULL, 0)) {
        label[0] = L'\0';
    }
    volume->label = wide_duplicate(label);
    return volume->label != NULL;
}

static int append_volume(DeviceInventory *inventory, VolumeInfo *volume) {
    VolumeInfo *new_values;
    size_t new_capacity;

    if (inventory->count == inventory->capacity) {
        new_capacity = inventory->capacity == 0 ? 8 : inventory->capacity * 2;
        if (new_capacity < inventory->capacity ||
            new_capacity > ((size_t)-1) / sizeof(VolumeInfo)) return 0;
        new_values = (VolumeInfo *)realloc(
            inventory->volumes, new_capacity * sizeof(VolumeInfo));
        if (new_values == NULL) return 0;
        inventory->volumes = new_values;
        inventory->capacity = new_capacity;
    }
    inventory->volumes[inventory->count++] = *volume;
    volume_init(volume);
    return 1;
}

static AppStatus scan_volume(
    DeviceInventory *inventory,
    const wchar_t *volume_guid,
    AppError *error)
{
    VolumeInfo volume;
    wchar_t *open_path;
    HANDLE handle;
    DWORD code;

    volume_init(&volume);
    code = ERROR_SUCCESS;
    volume.volume_guid = wide_duplicate(volume_guid);
    open_path = volume_open_path(volume_guid);
    if (volume.volume_guid == NULL || open_path == NULL) {
        free(open_path);
        volume_dispose(&volume);
        set_error(error, APP_OUT_OF_MEMORY, ERROR_NOT_ENOUGH_MEMORY, L"copy volume path");
        return APP_OUT_OF_MEMORY;
    }

    handle = CreateFileW(open_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    free(open_path);
    if (handle == INVALID_HANDLE_VALUE) {
        inventory->skipped_transient++;
        volume_dispose(&volume);
        return APP_OK;
    }
    if (!query_storage(handle, &volume, &code)) {
        CloseHandle(handle);
        volume_dispose(&volume);
        if (code == ERROR_NOT_ENOUGH_MEMORY) {
            set_error(error, APP_OUT_OF_MEMORY, code, L"query storage descriptor");
            return APP_OUT_OF_MEMORY;
        }
        inventory->skipped_transient++;
        return APP_OK;
    }
    CloseHandle(handle);

    if (volume.bus_type != BUS_TYPE_USB && volume.bus_type != BUS_TYPE_1394) {
        volume_dispose(&volume);
        return APP_OK;
    }
    if (!query_mounts(volume_guid, &volume, &code) || volume.mount_count == 0) {
        volume_dispose(&volume);
        if (code == ERROR_NOT_ENOUGH_MEMORY) {
            set_error(error, APP_OUT_OF_MEMORY, code, L"query volume mount points");
            return APP_OUT_OF_MEMORY;
        }
        inventory->skipped_transient++;
        return APP_OK;
    }
    if (!query_label(volume_guid, &volume)) {
        volume_dispose(&volume);
        set_error(error, APP_OUT_OF_MEMORY, ERROR_NOT_ENOUGH_MEMORY, L"copy volume label");
        return APP_OUT_OF_MEMORY;
    }
    volume.native_path = query_native_path(volume_guid);
    if (!append_volume(inventory, &volume)) {
        volume_dispose(&volume);
        set_error(error, APP_OUT_OF_MEMORY, ERROR_NOT_ENOUGH_MEMORY, L"append volume");
        return APP_OUT_OF_MEMORY;
    }
    return APP_OK;
}

static wchar_t *query_instance_id(DEVINST devinst) {
    ULONG length;
    wchar_t *value;

    length = 0;
    if (CM_Get_Device_ID_Size(&length, devinst, 0) != CR_SUCCESS ||
        length == 0 || length > 32767) return NULL;
    value = (wchar_t *)calloc((size_t)length + 1, sizeof(wchar_t));
    if (value == NULL) return NULL;
    if (CM_Get_Device_IDW(devinst, value, length + 1, 0) != CR_SUCCESS) {
        free(value);
        return NULL;
    }
    return value;
}

static void attach_interface_to_volumes(
    DeviceInventory *inventory,
    DWORD device_type,
    DWORD device_number,
    DEVINST disk_devinst)
{
    size_t index;
    DEVINST parent;
    CONFIGRET result;
    wchar_t *instance_id;

    parent = 0;
    result = CM_Get_Parent(&parent, disk_devinst, 0);
    if (result != CR_SUCCESS) parent = 0;
    instance_id = query_instance_id(parent != 0 ? parent : disk_devinst);

    for (index = 0; index < inventory->count; index++) {
        if (inventory->volumes[index].device_type == device_type &&
            inventory->volumes[index].device_number == device_number) {
            inventory->volumes[index].disk_devinst = disk_devinst;
            inventory->volumes[index].removal_devinst = parent;
            if (instance_id != NULL && inventory->volumes[index].instance_id == NULL) {
                inventory->volumes[index].instance_id = wide_duplicate(instance_id);
            }
        }
    }
    free(instance_id);
}

static void map_device_interfaces(DeviceInventory *inventory) {
    HDEVINFO set;
    DWORD index;
    SP_DEVICE_INTERFACE_DATA interface_data;
    SP_DEVINFO_DATA device_data;
    DWORD needed;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    HANDLE handle;
    STORAGE_DEVICE_NUMBER number;
    DWORD returned;

    set = SetupDiGetClassDevsW(&USB_EJECT_GUID_DEVINTERFACE_DISK,
        NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;

    for (index = 0; ; index++) {
        memset(&interface_data, 0, sizeof(interface_data));
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(set, NULL,
                &USB_EJECT_GUID_DEVINTERFACE_DISK, index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interface_data, NULL, 0, &needed, NULL);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) ||
            needed > 1024U * 1024U) continue;
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)calloc(1, needed);
        if (detail == NULL) break;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        memset(&device_data, 0, sizeof(device_data));
        device_data.cbSize = sizeof(device_data);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &interface_data,
                detail, needed, &needed, &device_data)) {
            free(detail);
            continue;
        }
        handle = CreateFileW(detail->DevicePath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            memset(&number, 0, sizeof(number));
            returned = 0;
            if (DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    NULL, 0, &number, sizeof(number), &returned, NULL) &&
                returned >= sizeof(number)) {
                attach_interface_to_volumes(inventory, number.DeviceType,
                    number.DeviceNumber, device_data.DevInst);
            }
            CloseHandle(handle);
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(set);
}

AppStatus inventory_build(DeviceInventory *inventory, AppError *error) {
    wchar_t volume_guid[MAX_PATH + 1];
    HANDLE find_handle;
    AppStatus status;
    DWORD code;

    app_error_clear(error);
    find_handle = FindFirstVolumeW(volume_guid, MAX_PATH + 1);
    if (find_handle == INVALID_HANDLE_VALUE) {
        code = GetLastError();
        set_error(error, code == ERROR_ACCESS_DENIED ? APP_ACCESS_DENIED : APP_INTERNAL_ERROR,
            code, L"FindFirstVolumeW");
        return error != NULL ? error->status : APP_INTERNAL_ERROR;
    }

    status = APP_OK;
    for (;;) {
        status = scan_volume(inventory, volume_guid, error);
        if (status != APP_OK) break;
        if (!FindNextVolumeW(find_handle, volume_guid, MAX_PATH + 1)) {
            code = GetLastError();
            if (code != ERROR_NO_MORE_FILES) {
                set_error(error, APP_INTERNAL_ERROR, code, L"FindNextVolumeW");
                status = APP_INTERNAL_ERROR;
            }
            break;
        }
    }
    FindVolumeClose(find_handle);
    if (status == APP_OK) map_device_interfaces(inventory);
    return status;
}

const wchar_t *inventory_bus_name(DWORD bus_type) {
    if (bus_type == BUS_TYPE_USB) return L"USB";
    if (bus_type == BUS_TYPE_1394) return L"IEEE 1394";
    return L"unknown";
}
