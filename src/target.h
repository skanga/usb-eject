#ifndef USB_EJECT_TARGET_H
#define USB_EJECT_TARGET_H

#include <stddef.h>

#include "app.h"
#include "cli.h"
#include "inventory.h"

typedef struct {
    size_t volume_index;
    DEVINST removal_devinst;
    size_t sibling_volume_count;
    size_t distinct_match_count;
} ResolvedTarget;

AppStatus target_resolve(
    const DeviceInventory *inventory,
    const TargetSelector *selector,
    ResolvedTarget *resolved,
    AppError *error);

#endif
