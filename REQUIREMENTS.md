# usb-eject Requirements

Status: Draft 0.3  
Target: MVP  
Date: 2026-09-03

## 1. Purpose

Create a small, native Windows command-line utility that lists removable storage volumes and safely requests their removal. The utility is a new C implementation derived from the behavior and Windows API approach of the original Delphi application.

The CLI is intended for interactive use, shortcuts, batch files, PowerShell, and portable application launchers.

## 2. MVP goals

The first release shall:

1. Build as a single 64-bit Windows executable with `..\mincc\petcc64.exe`.
2. Require no installation, service, driver, or non-system runtime DLL; normal enumeration and ejection shall work without mandatory elevation.
3. Enumerate USB and IEEE 1394 storage volumes that Windows considers present.
4. Show each volume's mount point, label, device name when available, bus type, and card-media status when known.
5. Safely eject a device selected by drive letter or mounted-folder path.
6. Select a device by volume label or device name.
7. Eject the device containing the running executable.
8. Eject removable media from a card reader without ejecting the reader itself.
9. When removal fails, identify every discoverable process and open resource associated with the target device rather than reporting only that the device is busy.
10. Clearly distinguish confirmed Windows veto information, likely blocking handles, and blockers that cannot be inspected.
11. When a user-mode process is a blocking candidate, offer an explicit way to terminate that process and retry the ejection.
12. Report failures through a clear diagnostic and a stable process exit code.
13. Use Unicode Windows APIs internally.

## 3. Non-goals for the MVP

The MVP shall not include:

- A graphical interface, tray icon, balloon notification, sound, or global hotkeys.
- Automatic background monitoring for device changes.
- Automatically closing or terminating applications without explicit user confirmation.
- Reading the original `USB_Disk_Eject.cfg` file.
- Persisted preferences or custom card-reader definitions.
- A 32-bit build.
- Windows XP support.
- Linux or macOS support.
- Closing or forcibly releasing an individual handle owned by another process.
- Forced removal after Windows vetoes a safe-removal request.

These features may be considered after the core enumeration and ejection behavior is proven reliable.

## 4. Supported environment

- Windows 10 and Windows 11, x64.
- Standard user accounts for normal enumeration and ejection. An elevated diagnostic pass may be needed to inspect handles owned by other users, services, or system processes.
- Local fixed, removable, USB, and IEEE 1394 storage may be discovered during enumeration, but only positively identified supported removable devices shall be offered for ejection.
- Network drives, optical drives, RAM disks, virtual disks, system volumes, and internal fixed disks are outside the MVP scope.

## 5. Command-line interface

Executable name used in this document: `usb-eject.exe`.

### 5.1 General syntax

```text
usb-eject.exe <command> [options]
```

Commands and option names shall be case-insensitive. Paths, labels, and device names shall preserve their original spelling for display.

The following commands are required:

```text
usb-eject.exe help
usb-eject.exe version
usb-eject.exe list
usb-eject.exe eject <target>
usb-eject.exe diagnose <target>
usb-eject.exe eject <target> --kill-blocker <pid>
usb-eject.exe eject --letter <letter>
usb-eject.exe eject --mount <path>
usb-eject.exe eject --label <label-or-pattern>
usb-eject.exe eject --name <name-or-pattern>
usb-eject.exe eject --this
```

`/?`, `-h`, and `--help` shall be aliases for `help`.

Help flags may appear anywhere after a recognized command and return help before
inventory or actions. `help list`, `help diagnose`, and `help eject` provide
command-specific selector syntax, option descriptions, and examples. Valued
options accept `--option=value`, including literal values beginning with `--`.
Missing values and unknown options identify the offending token and corrective syntax.

`diagnose` and `eject` accept `--verbose` for per-handle text details and
`--scan-timeout <ms>` for a traversal budget of 1..600000 ms (default 15000).
Console progress goes to stderr and is disabled for TSV. Header and payload
reads share the remaining per-handle/scan deadline. Windows metadata/enumeration
calls and cleanup are not promised a hard wall-clock deadline.

`diagnose` accepts the same target selectors as `eject`, but performs no removal request and makes no changes. It reports open resources that could prevent removal.

`--kill-blocker <pid>` may be repeated to authorize termination of specific blocker processes before one automatic ejection retry. It is valid only with `eject`; it is never implied by `--quiet` or any other option.

### 5.2 Target interpretation

For `eject <target>`:

- A single alphabetic character, optionally followed by `:`, is a drive letter.
- Any other value is treated as a filesystem path and resolved to its containing volume.

Exactly one explicit target selector may be supplied. Conflicting selectors are a usage error.

Drive letters and paths shall be normalized before matching. Mounted-folder paths shall work whether or not the user supplies a trailing backslash.

### 5.3 Label and name matching

Label and device-name comparisons shall be case-insensitive.

- Without `*`, the entire value must match.
- `*` may appear at the beginning, end, or both ends to request substring-style matching.
- A match against zero devices is an error.
- A match against more than one physical device is an ambiguity error. The program shall list the matches and shall not eject any of them.

### 5.4 Card-media ejection

The optional `--card` flag changes the operation from ejecting the parent device to issuing `IOCTL_STORAGE_EJECT_MEDIA` for the selected volume.

Card selection collapses partitions of the same disk/media, not separate media
that share a removable parent. Multiple matching media are ambiguous. Every
discovered volume on that media, including volumes without mount points, must
be locked with `FSCTL_LOCK_VOLUME` and flushed before the eject IOCTL. Any failed
lock, flush, or verification aborts removal and releases all acquired handles.
Unsupported media operations return 6; actual no-media errors return 8; other
I/O failures retain their own classification.

```text
usb-eject.exe eject E: --card
```

If the selected target has no media, does not support media ejection, or is not accessible, the command shall fail without falling back to ejecting the parent device.

### 5.5 List output

Default output is intended for people. It shall contain one entry per mounted supported volume and include, when available:

- Drive letter or mounted-folder path
- Volume label
- Vendor, product, and revision strings
- Bus type
- Whether the device appears to be a card reader
- Whether card media appears to be present
- A stable device-instance identifier for diagnostics

`list --format tsv` shall provide script-friendly tab-separated output with a single header row. Fields shall occur in this order:

```text
mount_point	label	vendor	product	revision	bus_type	card_reader	media_present	device_instance
```

Missing values shall be empty. Boolean values shall be `true`, `false`, or empty when unknown. Records shall be ordered case-insensitively by mount point.

JSON output is deferred until after the MVP.

Discovery errors must not be reported as a clean empty result. `list` emits the
available rows and returns 9 with explanatory stderr warnings if discovery was
incomplete. `eject` must refuse removal until full device scope is established.

## 6. Ejection behavior

For parent-device ejection, the implementation shall:

1. Resolve the target to a volume GUID path.
2. Open the volume and query `IOCTL_STORAGE_GET_DEVICE_NUMBER`.
3. Enumerate the appropriate storage device interfaces with SetupAPI.
4. Match the volume's storage device number to a device instance.
5. Resolve the device instance to the removable parent selected for safe removal.
6. Request removal with `CM_Request_Device_EjectW`.
7. Report a successful request only when Configuration Manager returns `CR_SUCCESS` without a veto.

The implementation may retry a transiently vetoed request up to three times with a short bounded delay. It shall report the final veto type and veto name when Windows supplies them.

If multiple mounted volumes belong to the selected parent device, the output shall state that the entire parent device is being ejected. A single ejection request shall be made for that parent.

The program shall not use `CM_Query_And_Remove_SubTree`, raw filesystem dismounts, surprise-removal simulation, or other forced-removal mechanisms in the MVP.

## 7. `--this` behavior

`eject --this` shall resolve the volume containing `usb-eject.exe`.

Because an executing image can prevent removal, the implementation shall support a two-stage portable mode:

1. Copy the executable to a uniquely named directory under the user's temporary directory.
2. Start the temporary copy with an internal continuation argument containing the original volume identity.
3. Exit the original process.
4. Allow a short bounded interval for the original image to close.
5. Request ejection from the temporary copy.
6. Arrange best-effort cleanup of the temporary executable and directory.

The child working directory is the new temporary directory. Temporary storage
and result receipts must resolve outside every volume in the removal scope.
The original returns 11 for a successful handoff, never 0. A new result receipt
is created before launching the child, with `state=pending`; final completion
appends `exit_code=N`. `--result-file <path>` chooses a new receipt for `--this`,
otherwise a unique receipt is generated under the temporary root. Existing files
are not overwritten. The receipt survives executable cleanup. Missing final
status means unknown/pending, not success. Launch failures after receipt creation
record their failure code in the receipt.

The internal continuation argument shall be documented in source but omitted from normal help output. It shall validate the supplied volume identity and reject malformed input.

## 8. Failure attribution and blocker diagnostics

Diagnosing a failed ejection is a core feature, not an optional verbose mode. When an ejection fails because the device is busy or Windows vetoes removal, the program shall immediately collect and display all available evidence explaining the failure.

### 8.1 Required evidence

The diagnostic result shall include:

- The Configuration Manager return value.
- The `PNP_VETO_TYPE` value and its symbolic name.
- The veto name returned by Windows, when supplied.
- Every discoverable open file or filesystem object on any volume belonging to the physical device.
- For each open object: its DOS path, native NT object path, object type, owning PID, process image path, and handle value.
- The process name, session ID, and user account when available.
- Hosted Windows service names when the owning process hosts one or more services.
- Processes whose executable image itself resides on the target device.
- The Windows error associated with every failed inspection operation.
- A diagnostic completeness status as defined below.

Diagnostics shall cover all sibling volumes that share the parent device, because an open resource on a different partition can veto removal of the entire device.

### 8.2 Evidence classification

Windows does not generally identify the individual handle that caused a Plug and Play removal veto. Therefore, the program shall not falsely label correlation as proof. Each finding shall be classified as one of:

- `confirmed-veto`: information returned directly by the failed removal request.
- `blocking-candidate`: an open resource found on the target device immediately after the failed request.
- `process-on-device`: a process whose executable image is located on the target device.
- `unresolved`: a process, service, driver, or handle that could not be inspected.

If Windows reports an outstanding-open veto and exactly one accessible open resource exists on the device, the resource is still a `blocking-candidate` unless Windows supplies a direct causal association.

### 8.3 Completeness status

Every diagnostic report shall end with one of these statuses:

- `complete`: every handle visible in the system handle snapshot was inspected or conclusively excluded, and no relevant process was inaccessible.
- `partial-access`: one or more processes or handles could not be inspected because of access restrictions or protected-process boundaries.
- `partial-timeout`: one or more object-name queries exceeded their bounded timeout.
- `kernel-or-driver`: Windows attributes the veto to a driver, device, or other kernel component for which no owning user-mode process can be reported.
- `changed-during-scan`: the process/handle state changed while diagnostics were being collected.
- `unresolved`: the available evidence does not explain the veto.

The tool shall never print that it found the exact blocker when the scan is incomplete. It shall state what could not be inspected and recommend rerunning the same command from an elevated terminal when elevation could improve coverage.

Elevation does not guarantee a complete result: protected processes, kernel-mode drivers, filesystem filter drivers, transient handles, and races may remain opaque. That limitation shall appear in `help` and in every affected report.

When run elevated, the tool shall be expected to resolve handles owned by ordinary desktop applications and non-protected Windows services across user sessions. Failure to identify a known test handle held by such a process is an MVP acceptance failure. Protected processes, secure processes, inaccessible kernel handles, and kernel/driver vetoes are explicit exceptions and must be reported as such.

### 8.4 Collection approach

The implementation shall:

1. Capture a system-wide handle snapshot as soon as practical after a failed removal request.
2. Resolve all volume GUID paths for the selected physical device to their native device paths.
3. Inspect accessible owning processes and duplicate candidate handles into the diagnostic process.
4. Resolve file handles with `GetFinalPathNameByHandleW` where possible and native object names where necessary.
5. Match resources against the target's complete set of DOS, volume-GUID, and native device paths using path-boundary-aware comparisons.
6. Correlate service PIDs through the Service Control Manager.
7. Bound potentially blocking native object queries so that one unusual handle cannot hang the CLI indefinitely.
8. Take a short verification snapshot when results changed during collection or when the first scan found no explanation for an outstanding-open veto.
9. Release all duplicated handles and process handles on every path.

Use of native NT handle-query APIs is permitted only inside an isolated compatibility module. Structure sizes and returned buffer bounds shall be validated at runtime. Failure of an unsupported native query shall produce an `unresolved` result rather than unsafe parsing.

### 8.5 Human-readable example

The failure output should follow this shape:

```text
Ejection failed: Windows vetoed removal (PNP_VetoOutstandingOpen)
Device: SanDisk Ultra USB Device
Volumes: E:, F:\Archive

Blocking candidates:
  PID 8420  C:\Program Files\Example\example.exe
    file     E:\documents\report.docx
    native   \Device\HarddiskVolume7\documents\report.docx
    handle   0x000000000000017C

Diagnostic completeness: complete
```

When no user-mode resource can be identified, the result shall say so explicitly and retain the veto or driver information, for example:

```text
Diagnostic completeness: kernel-or-driver
Windows named vetoing component: UASPStor
No owning user-mode process or open file could be associated with this veto.
```

### 8.6 Machine-readable diagnostics

`diagnose --format tsv` and failed `eject --format tsv` shall emit one record per finding with these fields:

```text
classification	pid	process_image	process_user	services	object_type	dos_path	nt_path	handle	win32_error	veto_type	veto_name	completeness
```

Diagnostic text belongs on standard error during `eject` so normal standard output remains usable by scripts. Standalone `diagnose` results belong on standard output.

### 8.7 Offering and terminating blocker processes

After reporting one or more live `blocking-candidate` or `process-on-device` findings, the tool shall offer a termination path:

- In an interactive console, it shall ask whether the user wants to terminate an eligible blocker and retry. The default answer is No.
- Each distinct eligible blocker is considered with fresh validation and its own confirmation; handling one does not stop the remaining offers.
- When input or output is redirected, it shall not prompt. It shall print the explicit `--kill-blocker <pid>` command needed to authorize a subsequent attempt.
- A command may use `--no-prompt` to disable interactive offers.
- Automated termination requires both `--kill-blocker <pid>` and `--yes`. Without `--yes`, each eligible PID shall still require interactive confirmation.

Before terminating a process, the program shall perform a fresh blocker scan and verify all of the following:

1. The PID still exists.
2. Its process creation time and executable image match the process originally diagnosed, preventing PID-reuse mistakes.
3. It still owns at least one blocking-candidate resource on the selected physical device, or its executable image is still located there.
4. The process is not `usb-eject.exe` itself, PID 0, PID 4, a critical process, a protected process, or a process whose safety status cannot be determined.
5. The authorized PID was supplied explicitly by the user; a stale PID copied from an earlier invocation must be diagnosed again before use.

The program should first offer a graceful close for an interactive desktop application by sending `WM_CLOSE` to its top-level windows and waiting for a bounded interval. If the process remains, it may offer forced termination. Services should first be offered a normal Service Control Manager stop when the user has permission. Forced process termination shall always carry a data-loss warning.

After an authorized close, service stop, or termination:

1. Wait for the process to exit with a bounded timeout.
2. Rescan the complete physical device for remaining blockers.
3. Print any blockers that remain.
4. Retry safe ejection exactly once if no unresolved safety condition was introduced.

Known remaining blockers and unresolved inspection errors prevent retry. Access,
timeout, or changing-handle limitations alone do not prevent the normal
Windows-vetoable parent removal retry; media removal must still acquire exclusive
locks. Recovery hints provide a complete quoted PowerShell command with the
resolved target, card mode, output preferences, scan budget, and explicit PID.

The tool shall never call a remote-close-handle operation. Termination acts on the owning process as a whole and may cause unsaved data loss.

## 9. Output and exit codes

Normal results and list data shall go to standard output. Diagnostics shall go to standard error.

The executable shall use these exit codes:

| Code | Meaning |
|---:|---|
| 0 | Operation completed successfully |
| 1 | Invalid command line or usage |
| 2 | No matching volume or device |
| 3 | More than one device matched |
| 4 | Removal was vetoed or the device is busy |
| 5 | Access denied |
| 6 | Unsupported target or operation |
| 7 | Windows API or internal error |
| 8 | Card reader contains no media |
| 9 | Diagnostic scan or device discovery was incomplete |
| 10 | Requested blocker process could not be safely stopped or terminated |
| 11 | Portable ejection started; read the receipt for the final outcome |

Every failure shall include a short explanation. Busy and vetoed removals shall additionally include the blocker diagnostic report from section 8.

Successful `eject` output shall identify the selected mount point and device. Scripts may suppress success output with `--quiet`; errors shall not be suppressed.

Target output includes the label and affected mounts (or GUIDs for unmounted
volumes). Diagnosis explicitly says it is read-only; card mode says the reader
is retained. Quiet applies to both initial success and retry success. Default
text diagnostics group findings by PID with counts and up to three resources;
`--verbose` shows all native paths and handles. TSV remains one row per finding.

## 10. Safety requirements

- No device shall be ejected when selection is ambiguous.
- The program shall never terminate another process without explicit PID-scoped authorization and confirmation as defined in section 8.7.
- The program shall never close, modify, or duplicate a handle with greater access than needed for inspection.
- The program shall not create or modify files on the selected volume; safe media preparation may flush already-cached filesystem data.
- An unsupported or insufficiently identified device shall be rejected rather than guessed.
- All Windows handles and device-information sets shall be released on every exit path.
- Buffer lengths returned by Windows shall be validated before use.
- No fixed-size buffer shall be used where a Windows API provides the required size dynamically.
- Human-readable device strings shall be treated as untrusted data when formatting output.
- Handle and process enumeration shall be observational only; diagnostics shall not suspend target processes.
- The tool shall refuse to terminate critical, protected, unverified, or no-longer-associated processes.
- `--yes` shall confirm only actions explicitly named on the same command line; it shall never turn on broad or inferred process termination.
- The temporary-copy mechanism shall operate only inside the resolved user temporary directory and shall use an unpredictable directory name.

## 11. Implementation constraints

- Language: C accepted by `petcc64`; avoid C++ syntax and dependencies.
- Character model: UTF-16 for Windows API boundaries; UTF-8 for redirected console output where practical.
- Build output: one PE console executable.
- Required system libraries are expected to include `kernel32`, `setupapi`, `cfgmgr32`, `advapi32`, and `ntdll`.
- Missing SDK declarations shall live in a small project-owned compatibility header, limited to APIs and structures actually used.
- The implementation shall not copy large portions of Windows SDK headers.
- The source shall keep enumeration, target matching, ejection, blocker diagnostics, native API compatibility, CLI parsing, and output formatting in separable modules even if the MVP is initially built as a unity translation unit.
- Compilation shall treat implicit function declarations and material type-conversion warnings as errors where supported by mincc.

A representative build command is:

```powershell
..\mincc\petcc64.exe -std -peconsole -Wall -Werror `
  -o build\usb-eject.exe src\usb_eject.c `
  -lkernel32 -lsetupapi -lcfgmgr32 -ladvapi32 -lntdll
```

The final flags may change as mincc compatibility is validated.

## 12. Acceptance criteria

The MVP is complete when all of the following are demonstrated:

1. A clean checkout builds with one documented command using the supplied mincc directory.
2. `help` and invalid-argument behavior match this document.
3. `list` finds an attached USB flash drive and emits valid text and TSV output.
4. Ejection by drive letter safely removes a single-volume USB flash drive.
5. Ejection by mounted-folder path safely removes a supported volume mounted without a drive letter.
6. Exact label and name selection work; ambiguous wildcard selection ejects nothing.
7. Selecting one partition of a multi-partition USB disk requests removal of the correct parent device once.
8. A test process holding a known file open produces exit code 4 and reports the process PID, executable, handle, and exact DOS and NT resource paths.
9. `--card` ejects media from at least one supported card reader without removing the reader device.
10. `--this` successfully ejects a USB drive containing the executable and leaves no persistent service or startup entry.
11. Internal fixed and system drives are rejected.
12. Repeated enumeration and failed ejection attempts show no handle growth under an external handle-monitoring tool.
13. A blocker on a sibling partition is reported when ejecting another partition of the same physical device.
14. An inaccessible or protected process produces an explicit incomplete diagnostic rather than a false claim that no blocker exists.
15. A service-hosted blocker reports both its host process and associated service name when Windows exposes that association.
16. A kernel- or driver-originated veto reports the Windows veto details and clearly states that no exact user-mode resource can be identified.
17. `diagnose` identifies open resources without issuing an eject, dismount, close-handle, or process-control operation.
18. In an elevated test, a normal process owned by another interactive user or an ordinary service holding a known file open is identified with its PID and exact resource path.
19. Interactive termination defaults to No, warns about data loss, and kills nothing when declined or when input is redirected.
20. `--kill-blocker <pid> --yes` terminates only a freshly revalidated eligible blocker, rescans, and retries safe ejection once.
21. PID reuse, a changed process image/creation time, a critical process, or a process no longer associated with the device causes termination to be refused.
22. A graceful window close or service stop is attempted before forced termination when applicable and selected by the user.

Hardware-dependent acceptance tests shall record the Windows version and the device vendor/product identifiers used.

## 13. Future considerations

Possible post-MVP work includes:

- x86 and ARM64 builds using a fuller toolchain.
- JSON output.
- Eject-all and explicit physical-device selection.
- Original INI-file compatibility and custom card-reader definitions.
- Optional Windows notifications.
- Automated tests around parsing, matching, formatting, and mocked Windows API responses.
- A second build configuration for MSVC or LLVM-MinGW with sanitizers and stronger diagnostics.
