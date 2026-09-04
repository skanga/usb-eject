#ifndef USB_EJECT_INVENTORY_H
#define USB_EJECT_INVENTORY_H

#include <windows.h>
#include <stddef.h>

#include "app.h"
#include "win32_compat.h"

typedef struct {
    wchar_t *volume_guid;
    wchar_t *native_path;
    wchar_t *label;
    wchar_t *vendor;
    wchar_t *product;
    wchar_t *revision;
    wchar_t *instance_id;
    wchar_t **mount_points;
    size_t mount_count;
    DWORD device_type;
    DWORD device_number;
    DWORD partition_number;
    DWORD bus_type;
    DEVINST disk_devinst;
    DEVINST removal_devinst;
    int removable_media;
    int media_present;
} VolumeInfo;

typedef struct {
    VolumeInfo *volumes;
    size_t count;
    size_t capacity;
    size_t skipped_transient;
} DeviceInventory;

void inventory_init(DeviceInventory *inventory);
void inventory_dispose(DeviceInventory *inventory);
AppStatus inventory_build(DeviceInventory *inventory, AppError *error);

int inventory_copy_descriptor_string(
    const unsigned char *descriptor,
    size_t descriptor_size,
    DWORD offset,
    wchar_t *output,
    size_t output_capacity);

const wchar_t *inventory_bus_name(DWORD bus_type);

#endif
