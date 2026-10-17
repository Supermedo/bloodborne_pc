"""Summarize actual displayed intervals from a PresentMon CSV, not FPS counters.

Supports PresentMon v1 and v2 schemas with case-independent column keys,
TimeInSeconds (converted to ms), TimeInMs, and CPUStartTime timestamps.
"""
import argparse
import csv
import math
import statistics
from collections import defaultdict


def number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (TypeError, ValueError):
        return None


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


class CaseInsensitiveDict:
    def __init__(self, d):
        self._d = {k.lower(): v for k, v in d.items() if k is not None}

    def get(self, key, default=None):
        return self._d.get(key.lower(), default)


def extract_timestamp_ms(row: CaseInsensitiveDict):
    t_sec = number(row.get("TimeInSeconds"))
    if t_sec is not None:
        return t_sec * 1000.0
    t_ms = number(row.get("TimeInMs"))
    if t_ms is not None:
        return t_ms
    cpu_t = number(row.get("CPUStartTime"))
    if cpu_t is not None:
        return cpu_t
    return None


def analyze_presentmon(csv_file, after_ms=0, before_ms=float("inf"), refresh_hz=None):
    chains = defaultdict(list)
    with open(csv_file, newline="", encoding="utf-8-sig") as source:
        for raw_row in csv.DictReader(source):
            row = CaseInsensitiveDict(raw_row)
            timestamp = extract_timestamp_ms(row)
            if timestamp is None or not (after_ms <= timestamp < before_ms):
                continue
            key = (row.get("Application", "Unknown"),
                   row.get("ProcessID", "0"),
                   row.get("SwapChainAddress", "0x0"))
            chains[key].append(row)

    if not chains:
        raise ValueError(f"no frames found in time window [{after_ms}, {before_ms}) ms")

    summary_results = []
    for key, rows in sorted(chains.items(), key=lambda item: len(item[1]), reverse=True):
        # Support case-insensitive msBetweenDisplayChange / MsBetweenDisplayChange
        displayed = [number(r.get("msBetweenDisplayChange")) for r in rows]
        intervals = [value for value in displayed if value is not None and value > 0]
        # Support case-insensitive msBetweenPresents / MsBetweenPresents
        presents = [number(r.get("msBetweenPresents")) for r in rows]
        presents = [value for value in presents if value is not None and value > 0]

        res = {
            "application": key[0],
            "pid": key[1],
            "swapchain": key[2],
            "submitted_rows": len(rows),
            "submitted_cadence_fps": (1000.0 / statistics.mean(presents)) if presents else None,
            "display_intervals_count": len(intervals),
            "display_cadence_fps": (1000.0 / statistics.mean(intervals)) if intervals else None,
            "display_mean_ms": statistics.mean(intervals) if intervals else None,
            "display_p50_ms": percentile(intervals, 0.5) if intervals else None,
            "display_p90_ms": percentile(intervals, 0.9) if intervals else None,
            "display_p99_ms": percentile(intervals, 0.99) if intervals else None,
            "present_modes": sorted({r.get("PresentMode", "?") for r in rows if r.get("PresentMode")}),
        }
        summary_results.append(res)

        print(f"{key[0]} PID {key[1]} swapchain {key[2]}: {len(rows)} submitted rows")
        if presents:
            print(f"  submitted cadence: {1000.0 / statistics.mean(presents):.2f} FPS")
            print("  submission interval ms: " + ", ".join(
                f"p{int(p*100)}={percentile(presents,p):.3f}" for p in (.1, .5, .9, .99)))
        if not intervals:
            print("  actual display timing unavailable; presentation rate alone does not prove smoothness")
            continue
        unavailable = sum(value is None for value in displayed)
        mean_int = statistics.mean(intervals)
        print(f"  actual display cadence: {1000.0 / mean_int:.2f} FPS, "
              f"{len(intervals)} intervals, {unavailable} rows without display timing")
        if refresh_hz and 1000.0 / mean_int > refresh_hz * 1.01:
            print(f"  above {refresh_hz:g} Hz: display changes can include torn scanouts; "
                  "this is not evidence of that many complete frames per second")
        print("  display interval ms: " + ", ".join(
            f"p{int(p*100)}={percentile(intervals,p):.3f}" for p in (.1, .5, .9, .99)))
        median = statistics.median(intervals)
        short = sum(value < median * .6 for value in intervals)
        long = sum(value > median * 1.5 for value in intervals)
        print(f"  intervals below 0.6x median: {short}; above 1.5x median: {long}")
        print("  present modes: " + ", ".join(res["present_modes"]))

    return summary_results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file")
    parser.add_argument("--after-ms", type=float, default=0)
    parser.add_argument("--before-ms", type=float, default=float("inf"))
    parser.add_argument("--refresh-hz", type=float, help="physical display refresh rate, for interpreting tearing-enabled captures")
    args = parser.parse_args()
    analyze_presentmon(args.csv_file, args.after_ms, args.before_ms, args.refresh_hz)


if __name__ == "__main__":
    main()
