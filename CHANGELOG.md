# Changelog

## 0.2.1

- Fix portable test setup on clean checkouts so release CI can complete.
- Includes all CLI and safe-removal improvements listed under 0.2.0; that
  tag's CI failed before publishing release binaries.

## 0.2.0

### Changed CLI behavior

- `eject --this` returns 11 when the temporary continuation starts. This is a
  pending operation, not a successful removal. Read the receipt's final
  `exit_code` line; only 0 means removal succeeded. Use `--result-file` to choose
  a new receipt path. Existing files are never overwritten.
- Incomplete discovery returns 9 with explanatory warnings. `list` preserves
  available rows; `eject` refuses an incompletely verified removal scope.
- Text diagnostics group resources by process. `--verbose` restores individual
  handles and native paths. TSV keeps one record per finding.

### Fixes and improvements

- Help works after a target, describes each command, and includes examples.
  Missing values identify the option; `--option=value` supports literal values
  beginning with `--`.
- Target summaries include labels and affected volumes, distinguish read-only
  diagnosis from removal, and correctly describe card mode.
- Card selection distinguishes separate media sharing a parent. All volumes on
  selected media are locked and flushed before removal; preparation failures
  prevent ejection. Unsupported, no-media, and I/O errors retain distinct codes.
- Interactive recovery considers every eligible blocker. Redirecting any standard
  stream disables prompts, and quiet mode also suppresses retry success output.
- Limited diagnostic coverage alone does not prevent a normal safe-removal retry.
  Process actions still require completed safety checks and PID-scoped consent.
- Recovery hints include complete PowerShell commands with safely quoted targets
  and preserved options.
- Portable continuations run from their temporary directory, outside the removal
  scope, and preserve a final-result receipt after executable cleanup.
- `--scan-timeout` sets the per-scan traversal budget, defaulting to 15000 ms.
  Console scans show progress. Helper header and payload reads share a deadline.

### Validation

Expanded automated tests cover CLI contracts, simulated recovery, device I/O,
portable launch/receipts, discovery failures, output formats, and deadlines.
Physical removal and card-reader firmware require dedicated hardware validation.
Individual Windows enumeration/metadata calls can exceed the traversal budget.
