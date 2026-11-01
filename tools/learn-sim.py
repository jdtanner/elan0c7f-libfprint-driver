#!/usr/bin/env python3
"""
Replay saved scans through a Python copy of the driver's matcher, with and
without learning, to choose or re-check the learning settings offline.

  sudo cat /var/lib/fprint/$USER/elan0c7f/0/7 > print.fp
  python3 -I learn-sim.py GAIN_H print.fp PROBE.pgm... --wrong OTHER.pgm...

GAIN_H is the driver's elan0c7f-gain.h.  PROBES are scans of the enrolled
finger IN THE ORDER THEY WERE MADE (learning is online, so order matters);
OTHERS are scans of other fingers.  For each setting it reports how many
probes would unlock, and the highest score any other finger reaches against
the enrollment plus everything learned.

The matcher is the same as elan0c7f-sift.cpp, icon correction included: on
the 2026-11-01 live session it reproduced every driver score exactly.  The
learning rule is the driver's: learn a probe that scores at least LEARN
against an ENROLLED sample (never a learned one), keep the newest CAP.

print.fp is fingerprint data: keep it private and delete it afterwards.

Requires python3-opencv, python3-numpy and python3-gobject.

Copyright (C) 2026 John Hill-Tanner
SPDX-License-Identifier: LGPL-2.1-or-later
"""

import re
import sys

import cv2
import numpy as np
from gi.repository import GLib

UPSCALE, RATIO, RANSAC_PX = 3, 0.8, 9.0
MIN_SCALE, MAX_SCALE = 0.8, 1.25
MATCH_THRESHOLD = 8
SETTINGS = [(None, 0), (20, 24), (15, 24), (15, 12), (12, 24)]


def load_gain(path):
    src = open(path).read()
    body = src[src.index("elan0c7f_gain_map["):]
    body = body[body.index("{") + 1: body.index("}")]
    vals = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]
    assert len(vals) == 52 * 150, len(vals)
    return np.array(vals, np.uint8).reshape(150, 52)


def extract(gain, path):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    if img is None:
        sys.exit(f"cannot read {path}")
    if img.shape == gain.shape:
        g = gain.astype(np.float32) / 255 + 0.5
        img = cv2.normalize(img.astype(np.float32) / g, None, 0, 255,
                            cv2.NORM_MINMAX, cv2.CV_8U)
    big = cv2.resize(img, None, fx=UPSCALE, fy=UPSCALE,
                     interpolation=cv2.INTER_CUBIC)
    eq = cv2.createCLAHE(2.0, (4, 4)).apply(big)
    kps, desc = cv2.SIFT_create().detectAndCompute(eq, None)
    xy = np.array([k.pt for k in kps], np.float64).reshape(-1, 2)
    if desc is None:
        desc = np.zeros((0, 128), np.float32)
    # The driver stores descriptors as bytes.
    desc = np.clip(desc, 0, 255).astype(np.uint8).astype(np.float32)
    return xy, desc


def score(a, b):
    (xa, da), (xb, db) = a, b
    if len(xa) < 2 or len(xb) < 2:
        return 0
    src, dst = [], []
    for m in cv2.BFMatcher(cv2.NORM_L2).knnMatch(da, db, k=2):
        if len(m) == 2 and m[0].distance < RATIO * m[1].distance:
            src.append(xa[m[0].queryIdx])
            dst.append(xb[m[0].trainIdx])
    if len(src) < 3:
        return 0
    model, inliers = cv2.estimateAffinePartial2D(
        np.float32(src), np.float32(dst), method=cv2.RANSAC,
        ransacReprojThreshold=RANSAC_PX)
    if model is None:
        return 0
    s = np.hypot(model[0, 0], model[1, 0])
    if s < MIN_SCALE or s > MAX_SCALE:
        return 0
    return int(np.count_nonzero(inliers))


def load_print(path):
    raw = open(path, "rb").read()
    if raw[:3] != b"FP3":
        sys.exit(f"{path} is not a libfprint print")
    v = GLib.Variant.new_from_bytes(
        GLib.VariantType.new("(issbymsmsia{sv}v)"), GLib.Bytes.new(raw[3:]),
        False)
    data = v.get_child_value(9)
    while data.get_type_string() == "v":
        data = data.get_variant()
    if data.get_type_string() != "(ua(aday))":
        sys.exit(f"{path} is not an elan0c7f SIFT print")
    samples = []
    for xy, desc in data.get_child_value(1).unpack():
        samples.append((np.array(xy, np.float64).reshape(-1, 2),
                        np.frombuffer(bytes(desc), np.uint8)
                        .reshape(-1, 128).astype(np.float32)))
    return samples


def main():
    args = sys.argv[1:]
    if len(args) < 3 or "--wrong" not in args:
        sys.exit(__doc__)
    i = args.index("--wrong")
    gain = load_gain(args[0])
    enrolled = load_print(args[1])
    probes = [extract(gain, p) for p in args[2:i]]
    wrong = [extract(gain, p) for p in args[i + 1:]]

    for learn, cap in SETTINGS:
        learned, unlocked, trace = [], 0, []
        for p in probes:
            e = max(score(p, s) for s in enrolled)
            hit = e >= MATCH_THRESHOLD or any(
                score(p, s) >= MATCH_THRESHOLD for s in learned)
            unlocked += hit
            trace.append("Y" if hit else ".")
            if learn and e >= learn:
                learned = (learned + [p])[-cap:]
        worst = max((score(w, s) for w in wrong for s in enrolled + learned),
                    default=0)
        name = "no learning" if learn is None else f"learn>={learn} cap={cap}"
        print(f"{name:20} {unlocked:3}/{len(probes)} unlocked  "
              f"wrong-finger max={worst}  {''.join(trace)}")


main()
