#!/usr/bin/env python3
"""
Prototype image-based matcher for the elan0c7f sensor, for comparison with
Bozorth3.  Writes the same N/S lines as score-matrix, so analyse.py can
evaluate it:

  python3 sift-score.py DATA_DIR/*/*.pgm > sift-scores.txt
  python3 analyse.py --thresholds=... sift-scores.txt right-index

Method (in the spirit of SIGFM, used for other small sensors): SIFT
keypoints on a contrast-equalised, upscaled image; nearest-neighbour
descriptor matches filtered by Lowe's ratio test; then RANSAC fits one
rotation + translation (+ near-unit scale) to the matches.  The score is the
number of matches consistent with that single transform, so coincidental
look-alike texture, which matches in random directions, scores low.

"N" lines report keypoints per image in place of minutiae.

Requires python3-opencv and python3-numpy.

Copyright (C) 2026 John Hill-Tanner
SPDX-License-Identifier: LGPL-2.1-or-later
"""

import math
import sys

import cv2
import numpy as np

UPSCALE = 3          # SIFT finds too few features at 52 px wide
RATIO = 0.8          # Lowe's ratio test
RANSAC_PX = 3.0 * UPSCALE
MIN_SCALE, MAX_SCALE = 0.8, 1.25   # same sensor, so scale stays near 1


def features(sift, clahe, path):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    if img is None:
        sys.exit(f"cannot read {path}")
    img = cv2.resize(img, None, fx=UPSCALE, fy=UPSCALE,
                     interpolation=cv2.INTER_CUBIC)
    img = clahe.apply(img)
    return sift.detectAndCompute(img, None)


def score(matcher, a, b):
    (kp_a, des_a), (kp_b, des_b) = a, b
    if des_a is None or des_b is None or len(kp_a) < 2 or len(kp_b) < 2:
        return 0
    good = [m for m, n in (p for p in matcher.knnMatch(des_a, des_b, k=2)
                           if len(p) == 2)
            if m.distance < RATIO * n.distance]
    if len(good) < 3:
        return 0
    src = np.float32([kp_a[m.queryIdx].pt for m in good])
    dst = np.float32([kp_b[m.trainIdx].pt for m in good])
    model, inliers = cv2.estimateAffinePartial2D(
        src, dst, method=cv2.RANSAC, ransacReprojThreshold=RANSAC_PX)
    if model is None:
        return 0
    s = math.hypot(model[0, 0], model[1, 0])
    if not MIN_SCALE <= s <= MAX_SCALE:
        return 0
    return int(inliers.sum())


def main():
    paths = sys.argv[1:]
    if len(paths) < 2:
        sys.exit(__doc__)
    sift = cv2.SIFT_create()
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(4, 4))
    matcher = cv2.BFMatcher(cv2.NORM_L2)

    feats = [features(sift, clahe, p) for p in paths]
    for i, (p, (kp, _)) in enumerate(zip(paths, feats)):
        print(f"N {i} {len(kp)} {p}")
    for i in range(len(paths)):
        for j in range(len(paths)):
            if i != j:
                print(f"S {i} {j} {score(matcher, feats[i], feats[j])}")


if __name__ == "__main__":
    main()
