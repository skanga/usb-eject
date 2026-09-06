# usb-eject

This repository contains a native command-line implementation inspired by
[USB Disk Ejector](https://github.com/bgbennyboy/USB-Disk-Ejector). It targets
64-bit Windows 10 and Windows 11 and builds with the `mincc` compiler.

## Install

Download `usb-eject.exe` and `usb-eject.exe.sha256` from the
[latest GitHub release](https://github.com/skanga/usb-eject/releases/latest),
verify the checksum, and place the executable in a directory on your `PATH`.
The program is portable: it installs no service, driver, or runtime.

## Quick start

```powershell
usb-eject.exe list
usb-eject.exe diagnose E:
usb-eject.exe eject E:
```

Targets can also be selected explicitly:

```powershell
usb-eject.exe eject --letter E
usb-eject.exe eject --mount "C:\Mounts\Camera"
usb-eject.exe eject --label "Work Backup"
usb-eject.exe eject --name "SanDisk*"
usb-eject.exe eject --this
```

`--this` copies the executable to a uniquely named temporary directory, exits
the original process, and continues from that directory. Both the temporary
directory and the result receipt must be outside the media/device being removed.
The continuation cleans up its temporary executable after it exits.
If the calling shell's working directory is on the USB drive, change to a local
directory before invoking the executable by its full path; the shell itself can
otherwise remain a blocker.

**Portable ejection returns exit code 11 (started), not 0 (removed).** Its receipt
initially contains `state=pending`; completion appends `exit_code=<number>`.
Only a final `exit_code=0` indicates success. A missing final line means the
outcome is unknown, including if the continuation was interrupted. Receipts
remain available after temporary executable cleanup and may be deleted after review.
Use a new `--result-file` path for scripts; existing files are never overwritten:

```powershell
$receipt = Join-Path $env:TEMP ("usb-eject-" + [guid]::NewGuid() + ".result")
usb-eject.exe eject --this --no-prompt --result-file $receipt
if ($LASTEXITCODE -ne 11) { throw "Ejection did not start: exit $LASTEXITCODE" }
$deadline = [DateTime]::UtcNow.AddMinutes(2)
do {
    $final = Get-Content -LiteralPath $receipt | Select-String '^exit_code=\d+$'
    if ($final) { break }
    Start-Sleep -Milliseconds 200
} while ([DateTime]::UtcNow -lt $deadline)
if (-not $final) { throw "No final result yet; inspect $receipt before unplugging" }
$ejectExitCode = [int]($final.Line -replace '^exit_code=', '')
if ($ejectExitCode -ne 0) { throw "Ejection failed: exit $ejectExitCode" }
```

Use `--card` to eject removable media without removing its reader. All volumes
on the selected media are locked and flushed before removal; a failed lock or
flush prevents ejection. On multi-slot readers, use a letter or mount path when
a name matches multiple media. Card preparation can require elevation. Use
`--no-prompt` to prohibit blocker-process prompts. Process termination is
always PID-scoped; unattended termination requires both
`--kill-blocker <pid>` and `--yes`.

Interactive recovery asks separately about each eligible process, tries graceful
close first, and separately confirms forced termination if necessary. Redirecting
any standard stream disables these prompts. After authorized actions, one
recovery retry is allowed when no known blockers or unresolved inspection errors
remain. Limited diagnostic access alone does not prevent a Windows-vetoable
parent removal request; card removal still requires exclusive volume locks.

Use `eject --help`, `diagnose --help`, or append `--help` to an existing command.
Values accept both `--option value` and `--option=value`; for example,
`diagnose --label=--backup` selects a label that begins with `--`.

Text diagnostics group resource findings by process. `--verbose` shows all
individual handles and native paths. `--scan-timeout <milliseconds>` sets a
per-scan traversal budget (default 15000, range 1 to 600000). The helper response
header and payload share a maximum one-second deadline, shortened to the
remaining scan budget. Progress goes to stderr in text console mode; no progress
is added to TSV. Native Windows enumeration/metadata calls and helper cleanup
can add time beyond the traversal budget. An interrupted scan reports incomplete
coverage rather than claiming there are no blockers.

`list --format tsv` and `diagnose <target> --format tsv` provide deterministic
machine-readable output. Failed `eject --format tsv` diagnostics are written
to standard error without human-readable text mixed into the TSV stream.
Backslash, tab, carriage return, and newline inside fields are escaped as
`\\`, `\t`, `\r`, and `\n`.

Discovery warnings go to stderr. `list` returns available rows with exit 9 when
discovery is incomplete; `eject` refuses removal when the full scope cannot be
verified. A clean empty list returns 0. `diagnose` also uses exit 9 for incomplete
coverage. Errors before an ejection request (usage, discovery, target resolution)
are explanatory text on stderr even when `--format tsv` was requested.

Run `usb-eject.exe help` for all options and exit codes. Normal enumeration and
parent-device ejection do not require elevation. An elevated terminal can improve diagnostic
coverage, although protected processes and kernel drivers may remain opaque.

## Build

Download the pinned
[minimalisti-C v0048 compiler](https://github.com/pducklin/minimalisti-C/releases/tag/v0048)
and extract `petcc64.exe` to an adjacent `mincc` directory. From this directory:

```powershell
.\build.ps1
```

The executable is written to `build\usb-eject.exe`.

GitHub Actions builds and tests every push and pull request. Successful builds
include a downloadable `usb-eject` artifact. Pushing a version tag
such as `v0.2.1` also creates a GitHub release containing `usb-eject.exe` and
its SHA-256 checksum file.

## Test

```powershell
.\tests\run.ps1
```

Unit tests do not eject hardware. The `list` command is read-only. Do not run
an `eject` command against media containing data that has not been saved.

Regression tests inject device I/O, process actions, discovery failures, console
state, and child launch behavior. They cover multiple blockers, limited scans,
quiet/redirected output, media ambiguity, locks/flush failures, portable receipts,
deadline expiry, help, and quoted recovery commands. The diagnostic smoke test
performs a read-only scan for a known open file. Real card-reader firmware and
physical safe removal still require dedicated hardware validation.

## Current implementation status

- Implemented: Unicode CLI parsing; deterministic text and TSV output;
  capability-checked USB/IEEE 1394 discovery; actionable ambiguity reporting;
  parent and card-media ejection; portable `--this` continuation; PnP veto,
  open-handle, and process-image diagnostics; supervised per-handle timeouts;
  process and service attribution; graceful close/service stop; separately
  confirmed forced termination; rescan; and one safe-removal retry.
- Card-reader identification is deliberately conservative. A positive result
  is based on removable-media capability plus recognizable reader metadata;
  otherwise the list reports `unknown` rather than guessing.

See [REQUIREMENTS.md](REQUIREMENTS.md) and [DESIGN.md](DESIGN.md) for the full
contract and implementation plan.

## License

This project is distributed under the GNU General Public License version 2.
See [LICENSE](LICENSE).
