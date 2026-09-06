# usb-eject Design

Status: Draft 0.1  
Requirements: [REQUIREMENTS.md](REQUIREMENTS.md), Draft 0.3  
Date: 2026-09-04

## 1. Design summary

`usb-eject.exe` will be a native, Unicode, 64-bit Windows console program written in C and built with `petcc64`. It will discover supported storage volumes, map them to physical removable device nodes, request safe removal, and investigate a failed removal by correlating the Windows PnP veto with system-wide open handles and processes.

The implementation has three safety boundaries:

1. Device selection is read-only until one unambiguous removable parent has been established.
2. Blocker inspection runs out of process so a stuck native object query cannot hang the main program.
3. Process termination is a separate, explicitly authorized state transition and requires a fresh blocker scan immediately before acting.

This is a new implementation. The original Pascal source is behavioral reference material; Delphi/VCL objects and third-party components are not carried into the CLI.

## 2. Goals of this design

This document explains how to implement the MVP requirements. It focuses on:

- Stable module and data boundaries suitable for C.
- Safe device enumeration and parent selection.
- Actionable failure attribution under elevation.
- Bounded behavior when Windows native queries hang or return unstable data.
- PID-scoped graceful close, service stop, and forced termination.
- A build that works with mincc's small header and library set.
- Test seams that do not require real hardware for every code path.

## 3. Key decisions

### 3.1 Use C and explicit ownership

The mincc compiler accepts C, not C++. Every owning structure will have an `init`/`dispose` pair, and functions will use one cleanup path where practical. Dynamic arrays carry `count` and `capacity`; Windows strings carry a character count excluding the terminating NUL.

No compiler-specific exception mechanism, C++ RAII, or global mutable device inventory will be used.

### 3.2 Use documented APIs for device operations

Volume discovery, SetupAPI enumeration, Configuration Manager traversal, storage IOCTLs, process metadata, service enumeration, and process control will use documented Win32 APIs.

The only native NT dependency is system-wide handle enumeration and object-name/type inspection. Those calls are isolated behind `nt_native.c`, loaded dynamically from `ntdll.dll`, bounds-checked, and treated as an optional diagnostic capability. A native API mismatch must degrade to an incomplete report rather than affect ejection.

Microsoft explicitly warns that `NtQuerySystemInformation` may change or become unavailable. The design therefore does not use it for selecting or ejecting a device.

### 3.3 Treat handle correlation as evidence, not causal proof

`CM_Request_Device_EjectW` returns the authoritative `CONFIGRET`, PnP veto type, and context-dependent veto name. Windows does not associate that veto with a particular handle. A handle found on the same physical device immediately afterward is a `blocking-candidate`, not a confirmed cause.

### 3.4 Supervise risky handle inspection in a helper process

Calls that resolve arbitrary handle names can block in a filesystem or device driver. An internal helper mode performs those calls. The parent receives progress and results over a pipe, imposes a per-handle deadline, and may terminate only its own helper process if it stops responding.

The target application is never suspended. A timeout becomes diagnostic evidence and reduces completeness.

### 3.5 Prefer graceful shutdown over termination

Interactive desktop applications are first offered `WM_CLOSE`. Services are first offered `SERVICE_CONTROL_STOP` where safe and supported. `TerminateProcess` is available only after a warning, explicit PID authorization, a fresh blocker scan, process-identity verification, and critical/protected-process checks.

## 4. High-level architecture

```text
                         +-------------------+
argv / console --------> | CLI + coordinator |
                         +---------+---------+
                                   |
                      enumerate and resolve target
                                   |
                         +---------v---------+
                         | device inventory  |
                         | volumes + parents |
                         +----+----------+---+
                              |          |
                     list/diagnose       | eject
                              |          v
                              |   +------+-------+
                              |   | PnP ejector  |
                              |   +------+-------+
                              |          |
                              +----+-----+ veto/failure
                                   |
                         +---------v----------+
                         | blocker diagnostic |
                         +----+-----------+---+
                              |           |
                    metadata/services    | risky handle queries
                              |           v
                              |   +-------+--------+
                              |   | helper process |
                              |   +-------+--------+
                              |           |
                              +-----------+ findings
                                          |
                                optional authorized action
                                          |
                              +-----------v-----------+
                              | close/stop/terminate   |
                              | rescan and retry once  |
                              +-----------------------+
```

## 5. Proposed source layout

```text
cli/
  REQUIREMENTS.md
  DESIGN.md
  README.md                 build and usage instructions
  build.ps1                 reproducible mincc build
  src/
    main.c                  entry point and top-level cleanup
    app.h                   shared domain types and result codes
    cli.c / cli.h           argument parsing and command model
    inventory.c / inventory.h
                            volume and device discovery
    target.c / target.h     normalization and matching
    eject.c / eject.h       parent and card-media ejection
    diagnose.c / diagnose.h blocker scan orchestration
    inspect_worker.c        internal helper mode and protocol
    process.c / process.h   process identity and safety checks
    services.c / services.h service/PID correlation and stop
    action.c / action.h     close, stop, terminate, rescan, retry
    portable.c / portable.h `--this` continuation flow
    output.c / output.h     console, UTF-8, human and TSV output
    win32_compat.h          missing public SDK declarations
    nt_native.c / nt_native.h
                            native declarations and dynamic loading
    memory.c / memory.h     overflow-checked arrays and strings
  tests/
    test_main.c
    test_cli.c
    test_target.c
    test_paths.c
    test_output.c
    fake_os.c / fake_os.h
```

The first implementation may use fewer physical files, but dependencies must continue to point inward toward the domain types. `output.c` must not perform discovery, and `diagnose.c` must not directly terminate target processes.

## 6. Core data model

The following are conceptual declarations. Exact field types may change during compiler validation.

```c
typedef struct {
    wchar_t *data;
    size_t length;
    size_t capacity;
} WString;

typedef struct {
    WString path;              /* E:\ or mounted folder */
} MountPoint;

typedef struct {
    WString volume_guid_path;  /* \\?\Volume{...}\ */
    WString native_path;       /* \Device\HarddiskVolume... */
    WString label;
    MountPoint *mounts;
    size_t mount_count;
    DWORD device_number;
    DWORD device_type;
    DWORD partition_number;
    DEVINST disk_devinst;
    size_t parent_index;
    int media_present;         /* -1 unknown, 0 false, 1 true */
} Volume;

typedef struct {
    DEVINST removal_devinst;
    WString instance_id;
    WString vendor;
    WString product;
    WString revision;
    STORAGE_BUS_TYPE bus_type;
    int removable;
    int card_reader;
    size_t *volume_indexes;
    size_t volume_count;
} PhysicalDevice;

typedef struct {
    Volume *volumes;
    size_t volume_count;
    PhysicalDevice *devices;
    size_t device_count;
} DeviceInventory;
```

`parent_index` always refers into the owning inventory. Copying an individual `Volume` or `PhysicalDevice` by value is forbidden after inventory construction because it would make ownership ambiguous.

### 6.1 Stable identity

Within one invocation, a physical device is keyed by its selected removal `DEVINST`. For display and continuation across processes, it is identified by the Configuration Manager device instance ID plus the set of volume GUID paths.

`DEVINST` values and PIDs are invocation-local and must never be persisted as stable identities.

### 6.2 Command model

Argument parsing produces one immutable command structure:

```c
typedef enum { CMD_HELP, CMD_VERSION, CMD_LIST, CMD_DIAGNOSE, CMD_EJECT } CommandKind;
typedef enum { SEL_NONE, SEL_IMPLICIT, SEL_LETTER, SEL_MOUNT, SEL_LABEL, SEL_NAME, SEL_THIS } SelectorKind;

typedef struct {
    CommandKind command;
    SelectorKind selector;
    WString selector_value;
    int card_mode;
    int quiet;
    int no_prompt;
    int assume_yes;
    int format_tsv;
    DWORD *authorized_pids;
    size_t authorized_pid_count;
} Command;
```

Parsing performs syntax validation only. Device-dependent validation happens after inventory construction.

## 7. Memory, strings, and errors

### 7.1 Allocation

All allocation passes through `memory.c`. Size multiplication and growth are checked for overflow. A zero-size allocation has defined project behavior and is never passed through ambiguously to the C runtime.

Allocation failures return `APP_OUT_OF_MEMORY`; they do not partially populate an inventory or diagnostic report.

### 7.2 Strings and paths

- Windows-facing strings use UTF-16 and explicit `W` APIs.
- `WString.length` excludes NUL; owned strings are always NUL-terminated.
- Paths are normalized lexically for matching but original display spelling is retained.
- Drive roots normalize to uppercase `X:\`.
- Mounted-folder paths normalize separators and remove trailing separators except at a root.
- Native prefix comparisons choose the longest matching device prefix and require a path boundary, preventing `Volume1` from matching `Volume10`.
- `\\?\`, volume-GUID, DOS, and native NT forms are retained as aliases rather than destructively converted into a single lossy representation.

### 7.3 Error object

Internal functions return an `AppStatus` and may fill:

```c
typedef struct {
    AppStatus status;
    DWORD win32_error;
    CONFIGRET config_ret;
    NTSTATUS nt_status;
    const wchar_t *operation;  /* static identifier */
    WString context;
} AppError;
```

The first causal error is retained. Cleanup failures may be logged in verbose diagnostics but do not overwrite it.

## 8. Device inventory

### 8.1 Volume enumeration

`inventory_build` performs these steps:

1. Enumerate volume GUID paths with `FindFirstVolumeW` and `FindNextVolumeW`.
2. Query the required mount-point buffer size, allocate it, and call `GetVolumePathNamesForVolumeNameW`.
3. Open the volume GUID path without its trailing backslash using `CreateFileW`, desired access `0`, and read/write sharing.
4. Query `IOCTL_STORAGE_GET_DEVICE_NUMBER`.
5. Query `IOCTL_STORAGE_QUERY_PROPERTY` using `StorageDeviceProperty` and validate every descriptor string offset before reading it.
6. Read the volume label with `GetVolumeInformationW`.
7. Resolve the volume GUID path to a native device path for later diagnostics.

A volume that disappears mid-scan is recorded as a transient skip, not a fatal inventory error. Structural corruption, allocation failure, or API contract violation is fatal.

### 8.2 Mapping volumes to device instances

SetupAPI enumerates `GUID_DEVINTERFACE_DISK` interfaces currently present. For each interface:

1. Allocate `SP_DEVICE_INTERFACE_DETAIL_DATA_W` using the size returned by `SetupDiGetDeviceInterfaceDetailW`.
2. Open the interface path with desired access `0` and read/write sharing.
3. Query its `STORAGE_DEVICE_NUMBER`.
4. Match device type and device number to enumerated volumes.
5. Retain the associated `SP_DEVINFO_DATA.DevInst`.

The structure `cbSize` values are defined from the x64 ABI and guarded by compile-time size/offset assertions in `win32_compat.h`.

### 8.3 Choosing the removable parent

Starting at the disk interface's `DEVINST`, the code walks ancestors with `CM_Get_Parent` and gathers:

- Device instance ID.
- Device capabilities, including removable and eject-supported flags.
- Enumerator/bus information where available.
- Parent and child relationships needed to group sibling volumes.

The selected removal node must be on a supported USB or IEEE 1394 storage ancestry and must advertise removable/eject capability. Selection stops at the narrowest ancestor that represents the complete removable unit. If the code cannot establish one safe removal node, the device is listed as unsupported and cannot be ejected.

This is intentionally stricter than blindly ejecting the immediate parent.

### 8.4 Physical-device grouping

All volumes resolving to the same selected removal node are grouped under one `PhysicalDevice`. This grouping drives:

- Ambiguity checks.
- The warning that all sibling volumes will be removed.
- Diagnostic matching across every partition.
- One and only one parent ejection request.

### 8.5 Card-media status

For likely card-reader volumes, `IOCTL_STORAGE_CHECK_VERIFY2` is issued on the volume handle opened with the least required access. The tri-state result distinguishes no media from an inability to determine media state.

`--card` opens the volume with the access required for `IOCTL_STORAGE_EJECT_MEDIA`. Failure never falls through to parent-device removal.

## 9. Target resolution

Target resolution consumes an immutable inventory and returns a physical-device index plus, where relevant, a volume index.

- Letter and mount selectors compare normalized mount points.
- An implicit path is resolved with `GetFullPathNameW` and then associated with its volume using `GetVolumePathNameW` and `GetVolumeNameForVolumeMountPointW`.
- `--this` begins with `GetModuleFileNameW` and uses the same path-to-volume resolution.
- Label matching considers volumes; results collapse by physical parent before deciding ambiguity.
- Name matching compares normalized combinations of vendor/product/revision and the display device name.
- Wildcard support is limited to the leading/trailing `*` forms defined by the requirements; no general glob engine is used.

An ambiguity response prints all distinct physical-device matches and performs no operation.

## 10. Ejection state machine

```text
RESOLVE_TARGET
    | unsupported/ambiguous -> REPORT_AND_EXIT
    v
VALIDATE_DEVICE
    | card mode ----------------> LOCK_ALL_MEDIA_VOLUMES -> FLUSH -> EJECT_CARD -> REPORT
    v
REQUEST_PARENT_EJECT
    | success ------------------> REPORT_SUCCESS
    | pending-close veto --------> bounded retry
    | any final veto/failure ----> DIAGNOSE
                                      |
                          no authorized action -> REPORT_FAILURE
                                      |
                          explicit action selected
                                      v
                              REVALIDATE_BLOCKER
                                      |
                                 CLOSE/STOP/KILL
                                      |
                                  RESCAN_DEVICE
                                      |
                              RETRY_EJECT_ONCE -> REPORT
```

`CM_Request_Device_EjectW` is always called with non-NULL veto outputs so Windows does not display its own UI. Success requires `CR_SUCCESS`. Veto type and name are initialized before every attempt so stale data cannot be reported.

Only `PNP_VetoPendingClose` receives automatic bounded retries before diagnostics: two additional attempts after approximately 250 ms and 750 ms. An outstanding-open, app, service, driver, device, rights, or unknown veto proceeds directly to diagnosis.

## 11. Blocker diagnostic design

### 11.1 Diagnostic report model

```c
typedef enum {
    FINDING_CONFIRMED_VETO,
    FINDING_BLOCKING_CANDIDATE,
    FINDING_PROCESS_ON_DEVICE,
    FINDING_UNRESOLVED
} FindingClass;

typedef struct {
    FindingClass classification;
    DWORD pid;
    ULONGLONG process_creation_time;
    WString process_image;
    WString process_user;
    WString *services;
    size_t service_count;
    WString object_type;
    WString dos_path;
    WString nt_path;
    ULONG_PTR remote_handle;
    DWORD granted_access;
    DWORD win32_error;
    NTSTATUS nt_status;
} Finding;

typedef struct {
    CONFIGRET config_ret;
    PNP_VETO_TYPE veto_type;
    WString veto_name;
    Finding *findings;
    size_t finding_count;
    unsigned issue_flags;
    DiagnosticCompleteness primary_completeness;
} DiagnosticReport;
```

The report owns all strings and remains valid after helper processes exit.

### 11.2 Target path set

Before scanning handles, diagnostics build a deduplicated alias set for every sibling volume:

- Each mount point.
- Each volume GUID path.
- Each `QueryDosDeviceW` native device prefix.
- Interface or device paths useful for display but not blindly treated as filesystem prefixes.

Matching is case-insensitive because Windows object-manager and filesystem paths used here are treated case-insensitively. A match requires equality or a separator boundary after the known root.

### 11.3 Native handle snapshot

`nt_native.c` resolves `NtQuerySystemInformation` at runtime and requests extended handle information. The buffer starts at a conservative size and grows only on the documented length-mismatch status, subject to an upper bound. The parser verifies:

- Returned byte count fits the allocation.
- Entry count multiplication cannot overflow.
- Every entry lies completely inside the returned buffer.
- PID and handle values fit the x64 model expected by this build.

If validation fails, scanning stops with `unresolved`; no entry is partially trusted.

### 11.4 Conclusively filtering handle types

Scanning every system handle would be slow and increases hang exposure. At startup of a diagnostic pass, the program calibrates the current boot's object-type indexes:

1. Open the current executable as a read-only file and identify that handle in a snapshot to learn the File type index.
2. Create a read-only image/file mapping of that file and identify the mapping handle to learn the Section type index.
3. Close both calibration handles.

Only File and Section handles can carry filesystem resources relevant to this design. Other type indexes are conclusively excluded and counted. If calibration fails, the scanner may query type names in the helper, but the completeness result cannot be `complete` unless all entries are classified.

### 11.5 Process metadata

For each PID owning a candidate handle type, the parent attempts to open the process with only:

- `PROCESS_QUERY_LIMITED_INFORMATION` for identity and metadata.
- `PROCESS_DUP_HANDLE` when handle inspection is needed.

It records creation time with `GetProcessTimes`, the image with `QueryFullProcessImageNameW`, session ID with `ProcessIdToSessionId`, token user SID/name when allowed, and whether the token is elevated.

The service map is captured once per diagnostic pass with `EnumServicesStatusExW(SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE)`, then joined by PID. A service silently omitted for lack of query rights contributes to `partial-access` if its PID is otherwise relevant.

All running process images are also compared with the target path set. This catches executables launched from the device even when their original file handle is not discoverable.

### 11.6 Inspector helper protocol

The executable's hidden `--internal-handle-worker` mode validates its inherited
input/output pipe handles. The parent duplicates only an already-inspected File
handle into the worker and sends that handle value. The worker resolves its
native path, closes the duplicate, and sends a fixed-size response header with
an error code and UTF-16 character count, followed by the optional path payload.
The receiver rejects oversized or unterminated paths.

Header and payload share one deadline: at most 1000 ms, shortened to the
remaining scan budget. The parent polls available pipe bytes before reading, so
an incomplete payload cannot strand a reader thread. A timed-out or failed worker
is closed/terminated and replaced for the next handle. Completed findings remain
in the report. No handles in other processes are remotely closed.

`--scan-timeout` sets a per-scan traversal budget (default 15000 ms). The scan
checks the budget between handle/process jobs and before service attribution;
exhaustion marks partial-timeout. Windows enumeration and metadata calls and
worker cleanup can add time beyond this cooperative budget. Text console scans
report progress on stderr approximately once per second. TSV and quiet mode
disable progress. Process actions require a fresh scan without timeout or
unresolved safety errors; enumeration-of-services failures also prevent action.

### 11.7 Snapshot consistency

Handle state is inherently racy. The report sets `changed-during-scan` when:

- A process creation time changes or a PID disappears.
- A snapshotted handle can no longer be duplicated.
- The duplicate has a different type than expected.
- A verification snapshot differs for relevant PID/handle pairs.

If the initial scan finds no blocking candidate after an outstanding-open veto, one verification snapshot is taken after a short delay. Results are deduplicated by process identity, remote handle value, object type, and normalized resource path.

### 11.8 Completeness calculation

Internally, completeness is a set of issue flags so information is not lost. Output selects one required primary status using this precedence:

1. `changed-during-scan`
2. `partial-timeout`
3. `partial-access`
4. `kernel-or-driver`
5. `unresolved`
6. `complete`

All secondary issues and counts are still printed. `kernel-or-driver` is selected only when the PnP veto identifies such a component and no more severe scan-integrity issue applies.

Elevation is detected from the process token. An elevated scan that cannot inspect an ordinary non-protected test process is a defect. Failure to inspect a protected/secure process or kernel-owned object is an explicit limitation, not a successful complete scan.

## 12. Blocker action design

### 12.1 Eligibility record

Diagnostic findings are collapsed into a process action record keyed by:

```text
PID + process creation time + canonical executable path
```

An action record contains its current matching findings, service associations, session, protection/critical status, and permitted actions. A PID alone never grants permission.

### 12.2 Interactive offer

The program considers prompting only if standard input and standard error are attached to console handles and `--no-prompt` is absent. Redirected or piped execution prints a suggested command but reads no input.

The prompt displays one process at a time and offers only eligible actions:

```text
[R]etry only  [C]lose gracefully  [K]ill process  [S]kip  [Q]uit
```

The default for an empty or unrecognized answer is Skip/No. Kill requires a second confirmation containing the PID and a data-loss warning. `--yes` bypasses that second prompt only for PIDs explicitly supplied by `--kill-blocker` on the same command line.

### 12.3 Fresh revalidation

Immediately before any action, the coordinator:

1. Opens the PID and compares `GetProcessTimes` creation time.
2. Requeries and compares the canonical executable path.
3. Rechecks critical and protection status.
4. Runs a focused fresh handle scan for that PID against the same physical device.
5. Requires at least one current blocker finding or a current process image on the device.

Any mismatch invalidates authorization and returns exit code 10 without acting.

### 12.4 Critical and protected processes

The safety check dynamically resolves the supported Windows process-critical/protection queries and opens the process with query rights before requesting terminate rights. The action is refused when:

- PID is 0, 4, or the current process.
- Windows marks the process critical.
- Windows reports a protected or secure process.
- Critical/protection state cannot be determined.
- The process hosts multiple active services.
- It is a system service host for which only a process-wide kill is possible.
- `PROCESS_TERMINATE` cannot be obtained.

An image-name denylist may provide defense in depth but is never the primary critical-process test.

### 12.5 Graceful desktop close

For an interactive application, `EnumWindows` selects top-level windows owned by the PID. `SendMessageTimeoutW(WM_CLOSE, SMTO_ABORTIFHUNG)` is used with a short timeout per window. The program then waits on the process handle for a bounded interval.

No window in another desktop/session is assumed reachable. If none can be closed, the output explains that limitation before offering forced termination.

### 12.6 Service stop

For a PID associated with exactly one active service, the program opens that service with `SERVICE_STOP | SERVICE_QUERY_STATUS`, verifies that it accepts stop, calls `ControlService`, and polls status with a bounded deadline.

A shared service host is never killed as a substitute for stopping one service. If multiple services share the PID, each association is reported and forced termination is refused.

### 12.7 Forced termination

After revalidation and confirmation, the program opens the same process identity with `PROCESS_TERMINATE | SYNCHRONIZE`, performs one final creation-time comparison, calls `TerminateProcess`, and waits for exit. The action result is printed before any rescan.

The program never calls `DuplicateHandle` with `DUPLICATE_CLOSE_SOURCE`.

### 12.8 Rescan and retry

After an action completes:

1. Rebuild enough inventory to prove that the same physical target is still present.
2. Run a complete blocker diagnostic scan.
3. If known blocking candidates or unresolved inspection errors remain, report them and do not retry automatically. Access, timeout, or changed-handle coverage limitations alone do not veto the normal Windows safe-removal retry.
4. Otherwise invoke the normal parent-removal path once (including its bounded pending-close handling), or reacquire exclusive media locks before card removal.

Each distinct eligible blocker receives a separate fresh validation and confirmation. The initial interactive pass does not stop after the first successful action. There is only one recovery retry after that pass.

## 13. Elevation behavior

The executable has an `asInvoker` manifest. It never silently elevates or starts an elevated helper. This preserves predictable stdout/stderr and avoids a second console window.

When access restrictions reduce diagnostic completeness, the report prints an exact rerun command and states that it should be executed from an elevated terminal. The command preserves target selection and output format but never carries forward `--yes` or a kill authorization automatically.

The inspector helper inherits the already-running parent's token. Consequently, a scan launched from an elevated terminal remains elevated without another prompt.

## 14. Portable `--this` flow

The original resolves the target, checks that the temporary root is outside its
removal scope, creates a random temporary subdirectory, and copies its executable.
It creates a new persistent result receipt outside the target, either at the
user's `--result-file` path or at a unique path under the temporary root. Creation
uses CREATE_NEW; an existing file is never overwritten. The initial ASCII receipt
contains `usb-eject-result-v1` and `state=pending` lines.

The child command carries the original PID, volume GUID, device instance ID,
resolved mount, receipt path, and execution options. Its working directory is
explicitly the temporary directory. The parent returns APP_STARTED (11), which
does not claim removal succeeded. It reports the receipt path even under quiet.

The continuation validates that it is running from an expected temporary-copy
path, parses the receipt/options, waits for the original PID to exit, rebuilds
inventory, and verifies the volume GUID and device instance before removal.
The final exit code is appended to the receipt and flushed; launch failures
after receipt creation also append their outcome. Missing completion remains
unknown/pending. Only `exit_code=0` establishes a successful removal result.

Cleanup uses the existing best-effort command script to remove the temporary
executable and its directory. The receipt remains outside that directory for
the caller to read and delete when no longer needed.

## 15. Output design

### 15.1 Console and redirected output

- If a stream is a console, output uses `WriteConsoleW`.
- If redirected, UTF-16 is converted with `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS)` and written with `WriteFile`.
- Human diagnostics from a failed `eject` go to standard error.
- Standalone `diagnose` findings go to standard output; invocation or scan errors go to standard error.

The program does not change the caller's console code page. Prompts require
all three standard streams to be consoles. Target summaries name the label and
every affected mount or unmounted-volume GUID; diagnosis is explicitly read-only
and card mode explicitly retains the reader. Default diagnostics group by PID
with counts and three example resources. `--verbose` retains per-handle details.
Quiet suppresses both first-attempt and retry success output. Recovery commands
use PowerShell single-quoted arguments (embedded quotes doubled) and preserve
the resolved mount, card mode, output options, scan budget, and explicit PID.
Discovery failures produce stderr warnings and exit 9 with available list data;
ejection refuses an incompletely established removal scope.

### 15.2 TSV escaping

TSV field values escape backslash, tab, carriage return, and newline as `\\`, `\t`, `\r`, and `\n`. Each logical finding occupies exactly one physical output line. Numeric error fields use decimal; handles use zero-padded hexadecimal.

Rows are buffered per record to reduce interleaving if standard output and error point to the same destination.

### 15.3 Determinism

Volumes sort by normalized mount point, then volume GUID. Diagnostic findings sort by PID, process creation time, classification, path, and handle. Ordering never relies on SetupAPI or system-handle enumeration order.

## 16. Win32 compatibility layer

`win32_compat.h` will contain only declarations absent from mincc's headers. Definitions will be transcribed from the Windows SDK/API contracts and grouped by source header:

- SetupAPI device-interface structures and functions.
- Configuration Manager types, capability constants, veto types, and functions.
- Storage property structures, bus types, and IOCTL values.
- Service enumeration/control structures and constants.
- Process-query and Job Object declarations needed by diagnostics.

Rules for this header:

- Prefer mincc's existing definition when present.
- Guard every addition with the same include guard or feature condition used by the project.
- Use fixed-width Windows ABI types, not host-dependent C guesses.
- Add compile-time assertions for every structure passed across an API boundary.
- Keep GUID definitions in one translation unit to avoid duplicate storage.
- Do not redefine broad portions of an SDK header.

Native structures used for the handle snapshot live separately in `nt_native.h`; they are never exposed to device-selection code.

Public system DLL imports are linked normally. Native NT entry points are obtained with `GetProcAddress` so their absence is detectable. The exact mincc library list will be established by a link probe before implementation proceeds.

## 17. OS abstraction and testing

### 17.1 Narrow injected API table

Logic-heavy modules receive an `OsApi` table of function pointers rather than calling global Windows imports directly. Production initializes it with real APIs; tests use fakes.

The table is split by capability (`VolumeApi`, `DeviceApi`, `ProcessApi`, `ServiceApi`) to avoid one enormous mock. Thin wrappers preserve `GetLastError` immediately after a failed call.

### 17.2 Unit tests

Unit tests cover:

- Argument conflicts, aliases, quoting, repeated PID options, and prompt rules.
- Drive, mounted-folder, volume-GUID, DOS, and native path normalization.
- Exact and permitted wildcard matching.
- Collapse of multiple matching volumes into one physical-device result.
- Ambiguity refusal.
- Descriptor offset and buffer-boundary validation.
- Handle snapshot overflow and truncation rejection.
- Native path prefix boundary matching.
- Helper protocol truncation, oversized lengths, stale PID, and timeout recovery.
- Completeness precedence and secondary issue reporting.
- TSV escaping and deterministic sorting.
- Critical/protected/shared-service action refusal.
- One-retry-only state-machine behavior.

### 17.3 Integration tests without removal

A test helper opens a known file, directory, and mapped section on a temporary test volume or ordinary local directory. Elevated and unelevated diagnostics verify PID, process identity, and exact path reporting. Destructive process tests launch a purpose-built expendable child that owns the resource.

Tests must never attempt to terminate an arbitrary pre-existing process.

### 17.4 Hardware tests

Manual or gated tests use disposable USB media and record device IDs and Windows build. Required cases are taken directly from the acceptance criteria, including:

- One-volume and multi-partition USB devices.
- A folder-mounted volume.
- Open files from the same and another user session.
- A dedicated stoppable test service.
- Card-reader media.
- Running `usb-eject.exe` from the target device.
- PnP vetoes that name applications, services, drivers, and devices where reproducible.

The test protocol verifies data integrity after reconnecting the device.

### 17.5 Secondary-toolchain validation

Although release builds use mincc, the same sources should periodically compile with MSVC or LLVM-MinGW at high warning levels. Those builds provide stronger static diagnostics and conventional debugger support. They are validation artifacts, not an MVP distribution requirement.

## 18. Build design

`build.ps1` resolves paths relative to its own location, creates `build` beneath `cli`, and invokes `..\..\mincc\petcc64.exe`. It never relies on the caller's current directory or global compiler installation.

The initial expected libraries are:

```text
kernel32 setupapi cfgmgr32 advapi32 user32
```

`ntdll` is loaded dynamically. Additional direct imports are added only when a link probe proves they are required.

The build fails on an implicit function declaration, a missing prototype, incompatible pointer conversion, or failed ABI assertion. Release and diagnostic builds differ only in optimization/debug flags and diagnostic logging; safety behavior is identical.

## 19. Logging and privacy

Default output contains only information necessary to identify devices and blockers. Full process paths, user names, service names, and resource paths are sensitive. The help text warns users before sharing diagnostic output.

There is no telemetry, network access, registry persistence, event-log write, or automatic upload. A future `--redact` option may replace user-profile prefixes and user names for support reports.

## 20. Failure handling

The coordinator follows these rules:

- A failure to enumerate enough information to prove safe device identity prevents ejection.
- A successful eject is never changed into failure by a later display or cleanup error.
- A failed diagnostic does not replace the original PnP veto; both are reported.
- Helper crash, malformed helper output, or helper timeout cannot crash the parent.
- If the target disappears during diagnosis, report that state and do not attempt process action.
- If the target identity changes between diagnosis and action, invalidate all authorizations.
- If output itself fails, stop interactive prompting and return an internal error unless the device was already successfully ejected.

## 21. Implementation stages

### Stage 1: Toolchain and ABI probes

- Verify each required DLL import with mincc.
- Validate compatibility structure sizes and offsets against a Windows SDK build.
- Prove Unicode console and redirected UTF-8 output.
- Prove the internal helper pipe/shared-memory protocol.

### Stage 2: Read-only inventory

- Implement memory/string/error infrastructure.
- Enumerate volumes and device interfaces.
- Group physical devices and produce text/TSV `list` output.
- Add parser, matcher, and inventory unit tests.

### Stage 3: Safe ejection

- Implement parent ejection and veto reporting.
- Implement card-media mode.
- Test only on disposable hardware.

### Stage 4: Diagnostics

- Implement native snapshot validation.
- Implement process/service metadata.
- Implement supervised handle inspection and path correlation.
- Establish elevated and unelevated completeness tests.

### Stage 5: Blocker actions

- Implement interactive gating and fresh revalidation.
- Add graceful desktop close and service stop.
- Add guarded forced termination.
- Add rescan and single retry.

### Stage 6: Portable mode and hardening

- Implement `--this` continuation and cleanup.
- Run leak, race, malformed-buffer, and device-disappearance tests.
- Validate with a secondary compiler and complete the hardware matrix.

## 22. Known risks

| Risk | Consequence | Mitigation |
|---|---|---|
| Native handle structures change | Diagnostics incomplete or unsafe parsing | Runtime bounds checks, isolated module, fail closed |
| Object query blocks in a driver | Diagnostic command hangs | Supervised helper process with progress deadline |
| Handle closes during scan | False or stale finding | Process identity checks and verification snapshot |
| Protected/kernel blocker | Exact process/resource unavailable | Explicit completeness status and PnP veto evidence |
| Incorrect removable-parent selection | Wrong device could be targeted | Strict ancestry/capability validation and physical grouping |
| PID reuse before termination | Wrong process could be killed | Creation-time and image revalidation immediately before action |
| Shared service host | Unrelated services could be killed | Allow SCM stop where attributable; refuse host-process kill |
| Incomplete mincc headers | ABI or link failure | Minimal compatibility declarations plus SDK cross-build assertions |
| Device removed during operation | Stale handles and identities | Treat disappearance as state change; stop further action |
| Diagnostic output leaks private paths | Sensitive support logs | Clear warning and future redaction option |

## 23. Deferred decisions

The following should be settled with prototypes rather than assumptions:

- Exact per-handle helper deadline and total scan deadline.
- Whether Section object names provide useful coverage on all supported Windows builds.
- The final algorithm for choosing the narrowest correct removable ancestor across unusual USB bridges.
- Whether custom card-reader definitions are still necessary on Windows 10/11.
- Whether an optional documented Restart Manager pass materially improves findings after exact file paths are known.
- Final release size and whether any compression is worth its antivirus false-positive risk.

## 24. Reference API contracts

- [CM_Request_Device_EjectW](https://learn.microsoft.com/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_request_device_ejectw)
- [PNP_VETO_TYPE](https://learn.microsoft.com/windows/win32/api/cfg/ne-cfg-pnp_veto_type)
- [NtQuerySystemInformation](https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation)
- [EnumServicesStatusExW](https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-enumservicesstatusexw)
- [File management APIs](https://learn.microsoft.com/windows/win32/api/fileapi/)
