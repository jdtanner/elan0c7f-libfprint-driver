#!/usr/bin/env python3
"""
Capture labelled fingerprint images from the elan0c7f sensor for offline
match analysis (see score-matrix.c and analyse.py).

Each capture goes through libfprint's normal capture path, so the saved
image is exactly what fprintd would extract minutiae from.

Usage (fprintd must not be using the sensor; root is needed for USB access):
  sudo systemctl stop fprintd
  sudo python3 capture.py DATA_DIR LABEL COUNT

LABEL names the finger, e.g. right-index, left-thumb.  Images are saved as
DATA_DIR/LABEL/NNN.pgm, continuing the numbering of any existing files.
Lift your finger fully and place it again between captures, varying the
position slightly as you would when unlocking.

Copyright (C) 2026 John Hill-Tanner
SPDX-License-Identifier: LGPL-2.1-or-later
"""

import os
import signal
import sys

import gi

gi.require_version("FPrint", "2.0")
from gi.repository import FPrint  # noqa: E402


def give_to_invoking_user(*paths):
    """Under sudo, leave the data owned by the user who ran sudo."""
    uid, gid = os.environ.get("SUDO_UID"), os.environ.get("SUDO_GID")
    if uid and gid:
        for p in paths:
            os.chown(p, int(uid), int(gid))


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    # capture_sync blocks in C, where Python's own Ctrl-C handler never runs.
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    data_dir, label, count = sys.argv[1], sys.argv[2], int(sys.argv[3])
    out_dir = os.path.join(data_dir, label)
    os.makedirs(out_dir, exist_ok=True)
    start = len([f for f in os.listdir(out_dir) if f.endswith(".pgm")])

    ctx = FPrint.Context()
    devices = [d for d in ctx.get_devices() if d.get_driver() == "elan0c7f"]
    if not devices:
        sys.exit("elan0c7f sensor not found (is fprintd still running?)")
    dev = devices[0]
    dev.open_sync()
    try:
        for n in range(start, start + count):
            print(f"[{label} {n - start + 1}/{count}] place finger...",
                  end="", flush=True)
            img = dev.capture_sync(True)
            w, h = img.get_width(), img.get_height()
            path = os.path.join(out_dir, f"{n:03d}.pgm")
            with open(path, "wb") as f:
                f.write(b"P5\n%d %d\n255\n" % (w, h))
                f.write(bytes(img.get_data()))
            give_to_invoking_user(out_dir, path)
            print(f" saved {path}")
    finally:
        dev.close_sync()


if __name__ == "__main__":
    main()
