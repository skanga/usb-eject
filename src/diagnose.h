#ifndef USB_EJECT_DIAGNOSE_H
#define USB_EJECT_DIAGNOSE_H

#include <windows.h>
#include <stddef.h>

#include "app.h"
#include "eject.h"
#include "inventory.h"
#include "target.h"

#define DIAG_ISSUE_ACCESS       0x01
#define DIAG_ISSUE_TIMEOUT      0x02
#define DIAG_ISSUE_CHANGED      0x04
#define DIAG_ISSUE_KERNEL       0x08
#define DIAG_ISSUE_UNRESOLVED   0x10

typedef enum {
    DIAGNOSTIC_COMPLETE = 0,
    DIAGNOSTIC_PARTIAL_ACCESS,
    DIAGNOSTIC_PARTIAL_TIMEOUT,
    DIAGNOSTIC_KERNEL_OR_DRIVER,
    DIAGNOSTIC_CHANGED_DURING_SCAN,
    DIAGNOSTIC_UNRESOLVED
} DiagnosticCompleteness;

typedef enum {
    DIAGNOSTIC_BLOCKING_CANDIDATE = 0,
    DIAGNOSTIC_PROCESS_ON_DEVICE,
    DIAGNOSTIC_CONFIRMED_VETO,
    DIAGNOSTIC_UNRESOLVED_FINDING,
    DIAGNOSTIC_SUMMARY
} DiagnosticClassification;

typedef struct {
    DiagnosticClassification classification;
    DWORD pid;
    ULONG_PTR handle_value;
    DWORD granted_access;
    DWORD session_id;
    ULONGLONG process_creation_time;
    wchar_t *process_image;
    wchar_t *process_user;
    wchar_t **services;
    size_t service_count;
    wchar_t *dos_path;
    wchar_t *nt_path;
    DWORD win32_error;
} BlockerFinding;

typedef struct {
    BlockerFinding *findings;
    size_t finding_count;
    size_t finding_capacity;
    unsigned issue_flags;
    size_t inaccessible_process_count;
    size_t changed_handle_count;
    size_t inspected_file_handle_count;
    int debug_privilege_enabled;
    DWORD last_win32_error;
    const wchar_t *last_operation;
    DiagnosticCompleteness completeness;
} DiagnosticReport;

DiagnosticCompleteness diagnostic_completeness_from_issues(unsigned issues);
const wchar_t *diagnostic_completeness_name(DiagnosticCompleteness value);
const wchar_t *diagnostic_classification_name(DiagnosticClassification value);
void diagnostic_sort_report(DiagnosticReport *report);
void diagnostic_report_init(DiagnosticReport *report);
void diagnostic_report_dispose(DiagnosticReport *report);
AppStatus diagnostic_scan(
    const DeviceInventory *inventory,
    const ResolvedTarget *target,
    DiagnosticReport *report,
    AppError *error);
int diagnostic_run_worker_if_requested(int *exit_code);

#endif
