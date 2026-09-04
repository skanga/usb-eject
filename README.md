# USB Eject CLI

This repository contains a native command-line implementation inspired by
[USB Disk Ejector](https://github.com/bgbennyboy/USB-Disk-Ejector). It targets
64-bit Windows 10 and Windows 11 and builds with the `petcc64` compiler in the
adjacent `mincc` directory.

## Build

From this directory:

```powershell
.\build.ps1
```

The executable is written to `build\usb-eject.exe`.

GitHub Actions builds and tests every push and pull request. Successful builds
include a downloadable `usb-eject-windows-x64` artifact. Pushing a version tag
such as `v0.1.0` also creates a GitHub release containing `usb-eject.exe` and
its SHA-256 checksum file.

## Test

```powershell
.\tests\run.ps1
```

Unit tests do not eject hardware. The `list` command is read-only. Do not run
an `eject` command against media containing data that has not been saved.

## Current implementation status

- Implemented: Unicode CLI parsing, text and TSV output, USB/IEEE 1394 volume
  discovery, target matching, ambiguity refusal, parent safe-removal request,
  card-media eject, PnP veto reporting, system-wide file-handle diagnostics,
  PID/image/user/session/resource attribution, service-name correlation,
  graceful application close, dedicated service stop, guarded PID termination,
  rescan, and one safe-removal retry.
- Not yet complete: supervised timeout isolation for unusual handle queries,
  mapped-section attribution, full card-reader classification, and portable
  temporary-copy continuation for `--this`. Until timeout isolation lands, a
  pathological filesystem driver could delay a diagnostic scan.

See [REQUIREMENTS.md](REQUIREMENTS.md) and [DESIGN.md](DESIGN.md) for the full
contract and implementation plan.

## License

This project is distributed under the GNU General Public License version 2.
See [LICENSE](LICENSE).
