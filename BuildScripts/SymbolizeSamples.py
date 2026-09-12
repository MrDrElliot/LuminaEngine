#!/usr/bin/env python3
"""Aggregate the sample dump written by LUMINA_BENCH_PROFILE_OUT into self and inclusive time.

The benchmarks in Engine/Source/Runtime/Tests/PackageLoadBenchmarks.cpp sample with SIGPROF and
write one line per sample, each frame as "<module>+0x<offset>". Offsets are module relative, so the
dump stays valid across runs and resolves here with addr2line.

    LUMINA_BENCH_PROFILE_OUT=prof.txt Binaries/Linux64/RuntimeTests-Editor-Development \
        --gtest_also_run_disabled_tests --gtest_filter='PackageLoadBenchmark.*'
    BuildScripts/SymbolizeSamples.py prof.txt [label]
"""

import collections
import os
import re
import subprocess
import sys

NOISE = ("OnProf", "backtrace", "__restore_rt", "__sigaction")
HARNESS = ("testing::", "main", "_start", "__libc_start_main_impl",
           "__libc_start_call_main", "call_init", "std::__cxx11::basic_string")


def read_samples(path, want_label):
    samples = []
    label = None
    for line in open(path):
        line = line.rstrip()
        if line.startswith("#"):
            label = re.match(r"# label (\S+)", line).group(1)
            continue
        if not line:
            continue
        if want_label is None or label == want_label:
            samples.append([tuple(tok.rsplit("+", 1)) for tok in line.split()])
    return samples


def resolve(samples):
    by_module = collections.defaultdict(set)
    for frames in samples:
        for module, offset in frames:
            by_module[module].add(offset)

    out = {}
    for module, offsets in by_module.items():
        if not os.path.exists(module):
            continue
        offsets = sorted(offsets)
        lines = subprocess.run(["addr2line", "-f", "-C", "-e", module] + offsets,
                               capture_output=True, text=True).stdout.splitlines()
        for i, offset in enumerate(offsets):
            name = lines[2 * i] if 2 * i < len(lines) else "??"
            where = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "??"
            out[(module, offset)] = (name, where)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    samples = read_samples(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    if not samples:
        print("no samples for that label")
        return 1

    names = resolve(samples)

    def useful(frames):
        kept, started = [], False
        for key in frames:
            entry = names.get(key)
            if entry is None:
                continue
            if not started:
                if any(n in entry[0] for n in NOISE):
                    continue
                started = True
            kept.append(entry)
        return kept

    self_time = collections.Counter()
    location = {}
    inclusive = collections.Counter()

    for frames in samples:
        stack = useful(frames)
        if not stack:
            self_time["<unresolved>"] += 1
            continue
        name, where = stack[0]
        self_time[name] += 1
        location[name] = where
        for name, _ in dict.fromkeys(stack):
            inclusive[name] += 1

    total = len(samples)
    print(f"total samples: {total}\n")
    print("=== self time ===")
    for name, count in self_time.most_common(25):
        print(f"{100.0 * count / total:6.2f}% {count:5d}  {name}")
        if name in location:
            print(f"                  {location[name]}")

    print("\n=== inclusive ===")
    for name, count in inclusive.most_common(60):
        if any(h in name for h in HARNESS):
            continue
        print(f"{100.0 * count / total:6.2f}% {count:5d}  {name}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
