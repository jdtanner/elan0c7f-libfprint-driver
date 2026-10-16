#!/usr/bin/env python3
"""
Analyse score-matrix output: how well does Bozorth3 separate the enrolled
finger from other fingers on this sensor?

Usage:
  score-matrix DATA_DIR/*/*.pgm > scores.txt
  python3 analyse.py scores.txt ENROLLED_LABEL [ENROLL_SAMPLES]

The image's parent directory name is its label (as written by capture.py).
ENROLLED_LABEL is the finger treated as enrolled, e.g. right-index; every
other label is an impostor.  ENROLL_SAMPLES (default 8, the driver's
nr_enroll_stages) is the size of each simulated enrollment.

Reports per-image minutiae counts, genuine vs impostor pair scores, and a
simulation of fprintd's decision rule: a probe matches when at least K of
the enrollment samples score >= T.  FRR = genuine probes rejected,
FAR = impostor probes accepted.

Copyright (C) 2026 John Hill-Tanner
SPDX-License-Identifier: LGPL-2.1-or-later
"""

import collections
import os
import random
import sys

TRIALS = 300
THRESHOLDS = [4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40]
KS = [1, 2, 3]


def load(path):
    labels, nrows, scores = {}, {}, {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if parts[0] == "N":
                i = int(parts[1])
                nrows[i] = int(parts[2])
                labels[i] = os.path.basename(os.path.dirname(parts[3]))
            elif parts[0] == "S":
                scores[(int(parts[1]), int(parts[2]))] = int(parts[3])
    return labels, nrows, scores


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else 0


def summary(name, xs):
    if not xs:
        return f"  {name:9s} (none)"
    nz = sum(1 for x in xs if x > 0)
    return (f"  {name:9s} n={len(xs):5d}  zero={100 * (len(xs) - nz) / len(xs):5.1f}%"
            f"  median={pct(xs, .5):3d}  p90={pct(xs, .9):3d}"
            f"  p99={pct(xs, .99):3d}  max={max(xs):3d}")


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    labels, nrows, scores = load(sys.argv[1])
    enrolled = sys.argv[2]
    n_enroll = int(sys.argv[3]) if len(sys.argv) == 4 else 8

    by_label = collections.defaultdict(list)
    for i, lab in labels.items():
        by_label[lab].append(i)
    if enrolled not in by_label:
        sys.exit(f"no images labelled {enrolled!r}; have {sorted(by_label)}")

    print("Images and minutiae kept for matching (-1 = rejected by libfprint):")
    for lab in sorted(by_label):
        ns = [nrows[i] for i in by_label[lab]]
        ok = [n for n in ns if n >= 0]
        print(f"  {lab:20s} images={len(ns):3d}  rejected={len(ns) - len(ok):2d}"
              f"  minutiae median={pct(ok, .5)} min={min(ok, default=0)}"
              f" max={max(ok, default=0)}"
              f"  below 5 (cannot score)={sum(1 for n in ok if n < 5)}")

    genuine = [s for (p, g), s in scores.items() if labels[p] == labels[g]]
    impostor = [s for (p, g), s in scores.items() if labels[p] != labels[g]]
    print("\nSingle pair scores (same finger vs different finger, all labels):")
    print(summary("genuine", genuine))
    print(summary("impostor", impostor))

    usable = [i for i in by_label[enrolled] if nrows[i] >= 0]
    impostors = [i for lab, idx in by_label.items() if lab != enrolled
                 for i in idx if nrows[i] >= 0]
    if len(usable) < n_enroll + 1:
        sys.exit(f"\nneed at least {n_enroll + 1} usable {enrolled} images "
                 f"to simulate enrollment, have {len(usable)}")

    rng = random.Random(0)
    frr = collections.Counter()
    far = collections.Counter()
    n_gen = n_imp = 0
    for _ in range(TRIALS):
        enroll = rng.sample(usable, n_enroll)
        rest = [i for i in usable if i not in enroll]
        for probe, bucket in ([(p, "g") for p in rest] +
                              [(p, "i") for p in impostors]):
            ss = [scores.get((probe, e), 0) for e in enroll]
            if bucket == "g":
                n_gen += 1
            else:
                n_imp += 1
            for t in THRESHOLDS:
                for k in KS:
                    hit = sum(1 for s in ss if s >= t) >= k
                    if bucket == "g" and not hit:
                        frr[(t, k)] += 1
                    if bucket == "i" and hit:
                        far[(t, k)] += 1

    print(f"\nSimulated decisions: enrolled={enrolled}, {n_enroll} samples per "
          f"enrollment, {TRIALS} random enrollments")
    print(f"  ({n_gen} genuine and {n_imp} impostor probe attempts in total)")
    print("  T = score threshold, K = samples that must reach it")
    print("  current driver: T=4, K=1\n")
    print("   T  " + "".join(f"   K={k}: FRR    FAR " for k in KS))
    for t in THRESHOLDS:
        row = f"  {t:2d}  "
        for k in KS:
            row += (f"      {100 * frr[(t, k)] / n_gen:5.1f}% "
                    f"{100 * far[(t, k)] / max(n_imp, 1):5.1f}%")
        print(row)


if __name__ == "__main__":
    main()
