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
    size_t *distinct_volume_indexes;
    int media_scope;
} ResolvedTarget;

void resolved_target_dispose(ResolvedTarget *resolved);
int target_same_media(const VolumeInfo *left, const VolumeInfo *right);
int target_volume_in_scope(const DeviceInventory *inventory,
    const ResolvedTarget *target, size_t index);
AppStatus target_resolve_mode(const DeviceInventory *inventory,
    const TargetSelector *selector, int media_scope,
    ResolvedTarget *resolved, AppError *error);

AppStatus target_resolve(
    const DeviceInventory *inventory,
    const TargetSelector *selector,
    ResolvedTarget *resolved,
    AppError *error);

#endif
