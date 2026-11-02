---
name: px4-bug-investigation
description: Investigate px4_drv reception, tuning, USB, signal-quality, and card-reader failures with evidence from each layer.
---

# px4_drv bug investigation

Use this skill when reception, tuning, signal statistics, USB recovery, card communication, or long-run stability behaves unexpectedly.

## Establish the test context

Record the product name, OS and build, tuner/receiver index, channel or RF frequency, modulation and symbol rate, bandwidth, antenna/feed path, B-CAS placement, application, test start/end time, and whether recording or viewing is active. Record D/E/S and the exact error time when the application exposes them.

For a live host, begin with read-only checks of processes, open clients, listeners, service state, active recordings, and recent logs. A service restart, process stop, capture, firmware write, channel change, card APDU, or configuration change can interrupt a recording; use those actions only when the user has explicitly authorized that specific impact.

## Collect evidence by layer

1. **Host and IPC:** Record device enumeration, selected definition, receiver index, client connect/disconnect, requested tune parameters, lock transitions, and the time to the first TS packet.
2. **USB and I2C:** Record endpoint, direction, request/address, requested and actual lengths, result code, elapsed time, and sequence number. Keep a bounded history around an error. Treat payload bytes as potentially containing card or service data; redact them before sharing a log.
3. **Demodulator and tuner:** Record the exact chip, mode, symbol rate, bandwidth, lock bits, raw C/N or AGC registers, FEC valid flags, and the register source/unit for each converted value.
4. **TS path:** Count bytes and packets, sync loss, TEI, per-PID continuity gaps, NULL packets, buffer overflow, and longest interval without input. Keep tuner lock separate from valid TS output.
5. **Card path:** Timestamp reader presence, reset/ATR, APDU result and duration, and card removal independently from TS continuity and descrambling status. Reader discovery or ATR success alone does not prove ECM or descrambling.

## Reproduce and narrow the layer

- First reproduce with the smallest safe operation and save the exact log/configuration and source revision.
- Compare one variable at a time. Useful separations include the same saved TS on another card reader, the same reader with a known-good saved TS, and live reception with card traffic disabled when that is safe and authorized.
- For tuning latency, measure command start, frequency programming completion, lock, stream start, and first valid 188-byte packet against a monotonic clock. Keep one absolute deadline for an operation; do not add retries or shorten waits without evidence from the target model.
- For TS quality, compare packets before and after host filters and distinguish source discontinuity, USB loss, host buffer overflow, and expected TSMF/PID filtering.
- For performance work, compare identical channels and duration, including the first seconds after tune and the steady-state interval. Report throughput, worst gap, CPU load, and all packet-error counters available.

## Change and verify

Trace the first incorrect value or state to its producer before proposing a fix. Write a failing synthetic test for protocol boundaries and state transitions where possible, then make the smallest change and rerun the relevant tests. Preserve Japanese/source evidence, device-specific waits, and existing cleanup contracts unless their reason has been disproved.

Report static analysis, automated tests, builds, simulations, and live-device observations as separate evidence. If hardware or a signal source is unavailable, identify the exact unresolved proof instead of describing a hypothesis as a fix.
