# usb-eject

This repository contains a native command-line implementation inspired by
[USB Disk Ejector](https://github.com/bgbennyboy/USB-Disk-Ejector). It targets
64-bit Windows 10 and Windows 11 and builds with the `petcc64` compiler in the
adjacent `mincc` directory.

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
the original process, and continues the ejection from the temporary copy. The
continuation cleans up its temporary files after it exits.

Use `--card` to eject removable media without removing its reader. Use
`--no-prompt` to prohibit blocker-process prompts. Process termination is
always PID-scoped; unattended termination requires both
`--kill-blocker <pid>` and `--yes`.

`list --format tsv` and `diagnose <target> --format tsv` provide deterministic
machine-readable output. Failed `eject --format tsv` diagnostics are written
to standard error without human-readable text mixed into the TSV stream.
Backslash, tab, carriage return, and newline inside fields are escaped as
`\\`, `\t`, `\r`, and `\n`.

Run `usb-eject.exe help` for all options and exit codes. Normal enumeration and
ejection do not require elevation. An elevated terminal can improve diagnostic
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
such as `v0.1.0` also creates a GitHub release containing `usb-eject.exe` and
its SHA-256 checksum file.

## Test

```powershell
.\tests\run.ps1
```

Unit tests do not eject hardware. The `list` command is read-only. Do not run
an `eject` command against media containing data that has not been saved.

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
