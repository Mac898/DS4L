#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (c) 2026 Macauley Lim
"""DualSense Bluetooth link-quality probe.

Timestamps input reports arriving on a DualSense hidraw node and reports
inter-arrival statistics: nominal report period, gap events (inter-arrival
above a threshold) and the spacing between gap events — to detect periodic
Bluetooth dropouts (e.g. the 2560 ms / 5-25 ms pattern caused by inquiry
scanning or Wi-Fi coex on another machine).

Safe to run alongside ds4ld: hidraw delivers a copy of every report to
each open file descriptor, so this steals nothing from the daemon.

Usage:
    bt_gap_probe.py /dev/hidrawN [duration_s] [--threshold-ms X] [--json]
"""

import collections
import json
import os
import select
import statistics
import sys
import time


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dev = sys.argv[1]
    duration = 30.0
    thr_ms = 5.0
    as_json = "--json" in sys.argv
    args = [a for a in sys.argv[2:] if not a.startswith("--")]
    if args:
        duration = float(args[0])
    if "--threshold-ms" in sys.argv:
        thr_ms = float(sys.argv[sys.argv.index("--threshold-ms") + 1])

    fd = os.open(dev, os.O_RDONLY)
    poller = select.poll()
    poller.register(fd, select.POLLIN)
    t0 = time.monotonic()
    stamps = []
    while True:
        now = time.monotonic()
        remaining = t0 + duration - now
        if remaining <= 0:
            break
        if not poller.poll(remaining * 1000):
            continue
        try:
            os.read(fd, 512)
        except OSError:
            break  # device disconnected — report what we collected
        stamps.append(time.monotonic())
    os.close(fd)

    if len(stamps) < 10:
        print(f"error: only {len(stamps)} reports in {duration}s — is the "
              "controller connected and streaming?", file=sys.stderr)
        return 1

    intervals = [(b - a) * 1000.0 for a, b in zip(stamps, stamps[1:])]
    median = statistics.median(intervals)
    mean = statistics.fmean(intervals)
    srt = sorted(intervals)

    def pct(p):
        return srt[min(len(srt) - 1, int(len(srt) * p))]

    # Gap events: anything past the threshold AND clearly off nominal
    # (2.5x median) — catches stalls, not ordinary jitter.
    gap_thr = max(thr_ms, median * 2.5)
    gaps = [
        (stamps[i] - t0, intervals[i])
        for i in range(len(intervals))
        if intervals[i] > gap_thr
    ]

    # Spacing between consecutive gap events, rounded to expose periodicity
    spacings = [
        round((g2[0] - g1[0]) * 10) / 10
        for g1, g2 in zip(gaps, gaps[1:])
        if g2[0] - g1[0] > 0.05
    ]
    spacing_hist = collections.Counter(spacings)

    report = {
        "device": dev,
        "duration_s": round(stamps[-1] - t0, 3),
        "truncated": len(stamps) > 0 and (t0 + duration - stamps[-1]) > 1.0,
        "reports": len(stamps),
        "rate_hz": round(len(stamps) / (stamps[-1] - t0), 1),
        "interval_ms": {
            "mean": round(mean, 3),
            "median": round(median, 3),
            "p95": round(pct(0.95), 3),
            "p99": round(pct(0.99), 3),
            "max": round(srt[-1], 3),
        },
        "gap_threshold_ms": round(gap_thr, 2),
        "gaps": [
            {"t_s": round(t, 3), "len_ms": round(l, 1)} for t, l in gaps
        ],
        "gap_count": len(gaps),
        "gap_spacing_hist_s": dict(sorted(spacing_hist.items())),
    }
    if as_json:
        print(json.dumps(report, indent=2))
    else:
        print(f"device           {dev}")
        if report["truncated"]:
            print("*** truncated: device disconnected mid-run ***")
        print(f"duration         {report['duration_s']}s")
        print(f"reports          {report['reports']} "
              f"({report['rate_hz']} Hz)")
        i = report["interval_ms"]
        print(f"interval ms      mean {i['mean']}  median {i['median']}  "
              f"p95 {i['p95']}  p99 {i['p99']}  max {i['max']}")
        print(f"gaps > {report['gap_threshold_ms']} ms: "
              f"{report['gap_count']}")
        for g in report["gaps"][:40]:
            print(f"  t={g['t_s']:8.3f}s  gap={g['len_ms']}ms")
        if report["gap_spacing_hist_s"]:
            print("gap spacing histogram (s -> count):")
            for k, v in report["gap_spacing_hist_s"].items():
                print(f"  {k}s x{v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
