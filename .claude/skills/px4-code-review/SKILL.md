---
name: px4-code-review
description: Review px4_drv changes for recording continuity, device parity, hardware-state ownership, and evidence quality.
---

# px4_drv code review

Use this skill for changes to `driver/`, `host/`, `winusb/`, `macos/`, shared protocol code, tests, or device documentation.

## Review sequence

1. Read the repository `AGENTS.md` and the relevant section of `docs/restructure_plan.md`. Check `git status --short` and keep pre-existing changes separate from the review target.
2. Read the full changed functions and their callers. Follow data from USB transfer through bridge, demodulator, tuner, TS synchronization, buffering, IPC, and published stats.
3. Compare the change with a working implementation and the history of the same path. Use `git log --follow -- <path>` and inspect the introducing commit where an existing wait, lock, reset, or asymmetric power rule looks unusual.
4. Check the tests and build definitions for each platform. A source file copied into a directory does not count as integrated until all intended Linux Kbuild, WinUSB project, macOS CMake, test, and packaging references are updated.
5. Review the evidence category for every important claim: source-proven, unit-tested, compiled, simulated, or verified with the named hardware and signal.

## Driver-specific checks

- **TS continuity:** Check 188-byte packet boundaries, tagged sync bytes, TEI, continuity counters, adaptation-field bounds, partial packets across transfers, channel-change reset, empty-read duration, and buffer overflow behavior.
- **Ownership and shutdown:** Trace references across USB removal, stream callbacks, named-pipe or socket clients, card sessions, shared power, and worker shutdown. Check every failure path for stale firmware, session, PID route, partial packet, or buffer state.
- **Locking and latency:** Record the existing lock order before evaluating a new lock. Check that USB waits, sleeps, logging I/O, and pipe I/O do not hold a TS or device-list lock. Treat added work in a high-rate callback as a latency change that needs measurement.
- **Hardware statistics:** Distinguish raw registers from converted C/N, RF level, BER, PER, packet bitrate, and theoretical bitrate. Verify the register unit, valid bit, denominator, lock prerequisite, counter reset, and overflow boundary from a primary source. Do not infer dBm from an uncalibrated AGC code or FEC enablement from a successful lock.
- **Device parity:** Compare the supported VID/PID and receiver systems across Linux USB tables, Windows and macOS definitions, the common factory, firmware/configuration files, and the test inventory. Pending devices remain a parity gap even when both host INI files list them.
- **Error handling:** Preserve USB, I2C, timeout, card absent, protocol mismatch, and device removal meanings across each API boundary. Retry only an operation known to be safe to repeat.
- **Logs:** Check level and category filtering, payload redaction, bounded storage, timestamps, and whether output can block a stream callback. Confirm that diagnostic settings are opt-in where they add work or expose device payload.
- **Packaging:** For Windows INF changes, verify CRLF without BOM and regenerate/verify catalogs from clean-checkout bytes. For public API changes, update both protocol version and compatibility documentation.

## Findings

Report actionable findings first, ordered by severity. Each finding includes the affected file and line, the failing condition, the user-visible recording or device impact, and the evidence that establishes it. Separate proven failures from risks that still need a test or hardware observation. If no actionable finding remains, state the exact diff and verification scope reviewed.

## Required validation summary

List commands, architectures, and results. Name the device model and connection conditions only when those conditions were actually exercised. A build or synthetic test does not establish hardware reception, long-run continuity, or support parity.
