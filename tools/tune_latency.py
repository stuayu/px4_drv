#!/usr/bin/env python3
"""Summarize DriverHost_PX4 Tune timing lines without connecting to a tuner."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, TextIO


_TUNE_MARKER = "Tune timing:"
_FIELD = re.compile(r"\b(system|freq_khz|set_ms|lock_ms|first_ts_ms)=(0[xX][0-9a-fA-F]+|[0-9]+)\b")


@dataclass(frozen=True)
class TuneSample:
    system: int
    frequency_khz: int
    set_ms: int
    lock_ms: int
    first_ts_ms: int


@dataclass(frozen=True)
class MetricSummary:
    median: float
    p95: int
    maximum: int


@dataclass(frozen=True)
class TuneSummary:
    system: int
    frequency_khz: int
    count: int
    set_ms: MetricSummary
    lock_ms: MetricSummary
    first_ts_ms: MetricSummary


def _message(line: str) -> str:
    stripped = line.lstrip()
    if stripped.startswith("{"):
        try:
            record = json.loads(stripped)
            if isinstance(record, dict) and isinstance(record.get("msg"), str):
                return record["msg"]
        except (json.JSONDecodeError, UnicodeDecodeError):
            pass
    return line


def parse_log_lines(lines: Iterable[str]) -> list[TuneSample]:
    """Read complete Tune timing records from text or JSONL DriverHost logs."""
    samples: list[TuneSample] = []
    for line in lines:
        message = _message(line)
        marker = message.find(_TUNE_MARKER)
        if marker < 0:
            continue

        fields = {name: value for name, value in _FIELD.findall(message[marker + len(_TUNE_MARKER):])}
        if len(fields) != 5:
            continue
        try:
            system_text = fields["system"]
            system = int(system_text, 16 if system_text.lower().startswith("0x") else 10)
            frequency = int(fields["freq_khz"], 10)
            set_ms = int(fields["set_ms"], 10)
            lock_ms = int(fields["lock_ms"], 10)
            first_ts_ms = int(fields["first_ts_ms"], 10)
        except (KeyError, ValueError):
            continue

        if not (0 <= system <= 0xffffffff and 0 <= frequency <= 0xffffffff):
            continue
        if min(set_ms, lock_ms, first_ts_ms) < 0:
            continue
        samples.append(TuneSample(system, frequency, set_ms, lock_ms, first_ts_ms))
    return samples


def _metric(values: list[int]) -> MetricSummary:
    ordered = sorted(values)
    p95_index = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return MetricSummary(statistics.median(ordered), ordered[p95_index], ordered[-1])


def summarize(samples: Iterable[TuneSample]) -> list[TuneSummary]:
    """Group samples by delivery system and frequency; p95 uses nearest rank."""
    groups: dict[tuple[int, int], list[TuneSample]] = {}
    for sample in samples:
        groups.setdefault((sample.system, sample.frequency_khz), []).append(sample)

    result = []
    for (system, frequency), group in sorted(groups.items()):
        result.append(TuneSummary(
            system=system,
            frequency_khz=frequency,
            count=len(group),
            set_ms=_metric([item.set_ms for item in group]),
            lock_ms=_metric([item.lock_ms for item in group]),
            first_ts_ms=_metric([item.first_ts_ms for item in group]),
        ))
    return result


def _open_logs(paths: list[str]) -> Iterable[tuple[str, TextIO]]:
    for name in paths or ["-"]:
        if name == "-":
            yield "<stdin>", sys.stdin
        else:
            stream = Path(name).open("r", encoding="utf-8-sig", errors="replace")
            yield name, stream


def _print_table(summaries: list[TuneSummary]) -> None:
    print("system   freq_khz  count  set_ms median/p95/max  lock_ms median/p95/max  first_ts_ms median/p95/max")
    for item in summaries:
        print(
            f"0x{item.system:08x} {item.frequency_khz:9d} {item.count:6d}  "
            f"{item.set_ms.median:g}/{item.set_ms.p95}/{item.set_ms.maximum}  "
            f"{item.lock_ms.median:g}/{item.lock_ms.p95}/{item.lock_ms.maximum}  "
            f"{item.first_ts_ms.median:g}/{item.first_ts_ms.p95}/{item.first_ts_ms.maximum}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="*", help="DriverHost log files (default: stdin)")
    parser.add_argument("--json", action="store_true", help="write the summary as JSON")
    args = parser.parse_args(argv)

    samples: list[TuneSample] = []
    try:
        for _, stream in _open_logs(args.logs):
            samples.extend(parse_log_lines(stream))
            if stream is not sys.stdin:
                stream.close()
    except OSError as exc:
        print(f"tune_latency: {exc}", file=sys.stderr)
        return 2

    summaries = summarize(samples)
    if not summaries:
        print("tune_latency: no complete Tune timing records found", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps([asdict(item) for item in summaries], ensure_ascii=False, indent=2))
    else:
        _print_table(summaries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
