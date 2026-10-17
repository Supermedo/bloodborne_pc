"""Summarize process memory samples from a gameplay stability run.

Exclude startup and deliberate loading/resize events with --after/--before.
A slope is descriptive evidence, not proof of a leak or of its absence.
Input CSV: seconds, private_bytes, working_set, handles, threads,
gpu_global_mib, gpu_util_percent. Global GPU memory includes other processes.
"""
import argparse
import csv
import math
import statistics


def summarize(rows, key, scale=1):
    samples = []
    for row in rows:
        try:
            t, value = float(row["seconds"]), float(row[key]) / scale
        except (KeyError, ValueError, TypeError):
            continue
        if math.isfinite(t) and math.isfinite(value):
            samples.append((t, value))
    if len(samples) < 2:
        return None
    samples.sort()
    times, values = zip(*samples)
    tm, vm = statistics.mean(times), statistics.mean(values)
    variance = sum((t - tm) ** 2 for t in times)
    if variance == 0:
        return None
    slope = sum((t - tm) * (v - vm) for t, v in samples) / variance * 60
    edge = max(1, len(samples) // 5)
    return dict(samples=len(samples), minutes=(times[-1] - times[0]) / 60,
                minimum=min(values), maximum=max(values), slope_per_minute=slope,
                early_median=statistics.median(values[:edge]),
                late_median=statistics.median(values[-edge:]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file")
    parser.add_argument("--after", type=float, default=300, help="seconds since launch")
    parser.add_argument("--before", type=float, default=float("inf"))
    args = parser.parse_args()
    with open(args.csv_file, encoding="utf-8-sig", newline="") as source:
        rows = []
        for row in csv.DictReader(source):
            try:
                t = float(row["seconds"])
            except (KeyError, ValueError, TypeError):
                continue
            if args.after <= t < args.before:
                rows.append(row)
    for key, scale, unit in (("private_bytes", 2**20, "MiB"),
                             ("working_set", 2**20, "MiB"),
                             ("gpu_global_mib", 1, "MiB (global)"),
                             ("handles", 1, "handles"), ("threads", 1, "threads")):
        result = summarize(rows, key, scale)
        if result is None:
            print(f"{key}: unavailable (fewer than two distinct sample times)")
            continue
        print(f"{key}: {result['samples']} samples over {result['minutes']:.2f} min; "
              f"range {result['minimum']:.2f}-{result['maximum']:.2f} {unit}; "
              f"early/late median {result['early_median']:.2f}/{result['late_median']:.2f}; "
              f"slope {result['slope_per_minute']:+.3f}/min")


if __name__ == "__main__":
    main()
