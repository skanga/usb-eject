#ifndef USB_EJECT_OUTPUT_H
#define USB_EJECT_OUTPUT_H

#include "app.h"
#include "diagnose.h"
#include "eject.h"
#include "inventory.h"
#include "target.h"

int output_write(int error_stream, const wchar_t *text);
int output_printf(int error_stream, const wchar_t *format, ...);
int output_help(void);
int output_command_help(CommandKind command);
int output_version(void);
int output_inventory(const DeviceInventory *inventory, OutputFormat format);
int output_target(const DeviceInventory *inventory, const ResolvedTarget *target);
int output_target_operation(const DeviceInventory *inventory,
    const ResolvedTarget *target, const Command *command);
int output_recovery_command(const Command *command, const VolumeInfo *volume, DWORD pid);
void output_set_verbose(int verbose);
int output_ambiguous(const DeviceInventory *inventory, const ResolvedTarget *target);
int output_app_error(const AppError *error);
int output_eject_error(const EjectResult *result);
int output_diagnostic(
    const DiagnosticReport *report,
    const EjectResult *eject_result,
    OutputFormat format,
    int error_stream);
int output_diagnostic_append(
    const DiagnosticReport *report,
    const EjectResult *eject_result,
    OutputFormat format,
    int error_stream);

#endif
