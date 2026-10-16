"""Summarize VK_NV_low_latency2 driver timestamps, not display/photon latency."""
import argparse
import json
import re
from pathlib import Path


PATTERN = re.compile(
    r"reflex timing: id=(\d+) input=(\d+) sim=(\d+)\.\.(\d+) "
    r"render=(\d+)\.\.(\d+) present=(\d+)\.\.(\d+) gpu=(\d+)\.\.(\d+)"
)
CPU_PATTERN = re.compile(
    r"reflex cpu: id=(\d+) sampled_ns=(\d+) present_ns=(\d+) latency_ms=([\d.]+)"
)


def summarize(text, tail_seconds=39):
    # Timings can be queried repeatedly. Use each completed real frame once.
    reports = {int(m[0]): tuple(map(int, m)) for m in PATTERN.findall(text)}
    rows = sorted(reports.values())
    if rows and tail_seconds:
        threshold = max(r[2] for r in rows) - tail_seconds * 1_000_000
        rows = [r for r in rows if r[2] >= threshold]
    invalid = 0
    metrics = {k: [] for k in ("simulation_to_gpu_end_ms", "input_to_gpu_end_ms",
                              "simulation_to_present_ms", "input_to_present_ms", "gpu_work_ms")}
    missing_input = 0
    for r in rows:
        _, inp, sim0, sim1, render0, render1, present0, present1, gpu0, gpu1 = r
        if (not all(r[2:]) or sim1 < sim0 or render1 < render0 or
                present1 < present0 or gpu1 < gpu0 or gpu1 < sim0 or present0 < sim0):
            invalid += 1
            continue
        metrics["simulation_to_gpu_end_ms"].append((gpu1 - sim0) / 1000)
        metrics["simulation_to_present_ms"].append((present0 - sim0) / 1000)
        metrics["gpu_work_ms"].append((gpu1 - gpu0) / 1000)
        if sim0 <= inp <= min(gpu1, present0):
            metrics["input_to_gpu_end_ms"].append((gpu1 - inp) / 1000)
            metrics["input_to_present_ms"].append((present0 - inp) / 1000)
        else:
            missing_input += 1

    def percentiles(values):
        if not values:
            return None
        values = sorted(values)
        return {f"p{p}": round(values[round((len(values) - 1) * p / 100)], 3)
                for p in (50, 95, 99)}

    # A guest input/simulation cycle logs its first pad sample once, but the
    # historical guest flips can present several times per cycle; generated
    # presentations do not emit CPU records. One id may have several real
    # presents. Group by (id, sample); collapse exact (id, sample, present)
    # duplicates that a queried cycle repeats verbatim; then drop records whose
    # sample or timestamps are invalid before any latency is measured.
    cpu_cycles = {}
    for fid, sample, present, _ in CPU_PATTERN.findall(text):
        cpu_cycles.setdefault((int(fid), int(sample)), set()).add(int(present))
    # The window is anchored on the newest present timestamp in the log, taken
    # before invalid records are removed, so trimming matches the raw stream.
    threshold = None
    if cpu_cycles and tail_seconds:
        threshold = max(max(presents) for presents in cpu_cycles.values()) - tail_seconds * 1e9

    first_values = []
    last_values = []
    repeated = 0
    input_cycles = 0
    for (_, sample), presents in cpu_cycles.items():
        if sample <= 0:
            continue
        valid = [p for p in presents
                 if p >= sample and (threshold is None or p >= threshold)]
        if not valid:
            continue
        first = min(valid)
        last = max(valid)
        input_cycles += 1
        # The tail window trims individual present timestamps, so a cycle whose
        # first present falls before the window reports its first in-window
        # present here. Its last present is unaffected, because the window is
        # anchored on the newest presented timestamp. Distributions, not
        # physical input-to-photon latency.
        first_values.append((first - sample) / 1e6)
        last_values.append((last - sample) / 1e6)
        repeated += len(valid) - 1

    return {"frames": len(rows), "invalid_reports": invalid,
            "missing_or_invalid_input": missing_input,
            **{k: percentiles(v) for k, v in metrics.items()},
            # cpu_owned_frames and cpu_input_to_present_ms keep the previous
            # last-present-per-cycle meaning. cpu_first_present_ms and the new
            # fields describe the same cycles from the input side; the parallel
            # names make repeats and both ends of the present span explicit.
            "cpu_owned_frames": len(last_values),
            "cpu_input_to_present_ms": percentiles(last_values),
            "cpu_first_present_ms": percentiles(first_values),
            "cpu_last_present_ms": percentiles(last_values),
            "cpu_repeated_present_records": repeated,
            "cpu_input_cycles": input_cycles,
            "gpu_windows_under_0_1_ms": sum(v < .1 for v in metrics["gpu_work_ms"])}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--tail-seconds", type=float, default=39)
    args = parser.parse_args()
    print(json.dumps(summarize(args.log.read_text(errors="replace"), args.tail_seconds), indent=2))
    print("Driver timestamps exclude monitor scanout and photon latency.")
