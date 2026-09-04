#ifndef USB_EJECT_EJECT_H
#define USB_EJECT_EJECT_H

#include "app.h"
#include "inventory.h"
#include "win32_compat.h"

typedef struct {
    AppStatus status;
    CONFIGRET config_ret;
    PNP_VETO_TYPE veto_type;
    wchar_t veto_name[MAX_PATH + 1];
    DWORD win32_error;
} EjectResult;

AppStatus eject_status_from_veto(CONFIGRET result, PNP_VETO_TYPE veto_type);
AppStatus eject_parent_device(DEVINST removal_devinst, EjectResult *result);
AppStatus eject_card_media(const VolumeInfo *volume, EjectResult *result);
const wchar_t *eject_veto_name(PNP_VETO_TYPE veto_type);

#endif
