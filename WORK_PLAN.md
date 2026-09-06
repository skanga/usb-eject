# CLI review implementation

Scope: all twelve review findings, the six usability improvements, and safe card-media removal. Preserve PID-scoped consent, Unicode, existing TSV columns, and normal Windows safe-removal APIs. Hardware removal and process termination are tested with injected fakes, never against the user's devices or applications.

Milestones:
- [x] CLI parsing, contextual errors and complete command help; regression tests.
- [x] Operation-aware target resolution, identity/scope output and grouped diagnostics with --verbose.
- [x] Blocker workflow: all eligible PIDs, redirected streams, bounded retry and --quiet; simulated orchestration tests.
- [x] Card locks, flush and error mapping; injected device-I/O tests.
- [x] Inventory completeness and actionable discovery errors.
- [x] Portable continuation working directory and final-result receipt, distinct pending exit code.
- [x] Diagnostic scan budget, progress and bounded helper reads.
- [x] README/contracts, end-to-end tests, build, final diff review.

Done when the test suite covers regressions and new CLI contracts, the release build passes, and each milestone is implemented and documented. Real card-reader and removal verification remains a separately identified hardware limitation.

Portable decision: retain the necessary asynchronous --this handoff, use exit 11 for a started operation (never success), and provide a persistent result receipt that can be polled. Parent-device retries remain Windows-vetoable despite diagnostic access limitations; card retries must acquire exclusive volume locks before removal.

Validation: parser and card regression failures reproduced before their fixes;
unit, CLI, simulated recovery, card-I/O, portable launch/receipt, inventory fault,
deadline and read-only handle-discovery tests pass through tests/run.bat. The
release executable builds through build.bat. Hardware ejection, reader firmware,
and destructive process actions were not exercised. The traversal deadline cannot
preempt individual Windows enumeration/metadata calls; this limit is documented.
