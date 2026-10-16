# ELAN 04f3:0c7f Fingerprint Reader — libfprint Driver

libfprint driver for the **ELAN Microelectronics 04f3:0c7f** fingerprint sensor,
found in Acer Swift laptops. Targets **libfprint 1.94.100**; tested on Fedora 45
(Wayland) with fprintd 1.94.5.

A full libfprint fork with these changes applied is on the `elan-0c7f-1.94.100`
branch of https://github.com/jdtanner/libfprint (the older `elan-0c7f-driver`
branch targets 1.94.10).

---

## What is in this repository

| File | Description |
|------|-------------|
| `libfprint/drivers/elan0c7f.c` | The driver source file |
| `libfprint/drivers/elan0c7f-sift.cpp`, `.h` | The driver's SIFT/RANSAC matcher (needs OpenCV) |
| `libfprint/drivers/elan0c7f-gain.h` | Built-in map of the power-key icon, divided out of every scan |
| `libfprint.patch` | Patch for supporting changes to libfprint core files |
| `gnome-control-center.patch` | Patch for two GNOME Settings fingerprint dialog bugs |
| `tools/` | Capture and offline match-accuracy tools (see below) |

---

## About the sensor

The 04f3:0c7f is an image-based sensor — it captures a raw 52×150 px pixel image
over USB bulk transfer and sends it to the host. All matching is done in software
on the host. It is **not** a match-on-chip device.

The sensor appears primarily in Acer Swift laptops (SF314-512, SFX14-51G,
SF514-56T, SFX16-52G) and has been probed on over 100 Linux systems, all
previously failing.

---

## Applying the driver

The steps below apply to a libfprint **v1.94.100** source tree.

### 1. Add the driver files

```
cp libfprint/drivers/elan0c7f.c libfprint/drivers/elan0c7f-sift.* \
   libfprint/drivers/elan0c7f-gain.h /path/to/libfprint/libfprint/drivers/
```

The driver needs OpenCV 4 (`opencv4` in pkg-config; `opencv-devel` on Fedora).

### 2. Apply the libfprint core patch

```
cd /path/to/libfprint
patch -p1 < /path/to/libfprint.patch
```

This patch covers:
- `libfprint/meson.build` and `meson.build` — register the new driver and its
  OpenCV dependency
- `libfprint/fpi-image-device.[ch]`, `libfprint/fp-image-device.c` — optional
  `print_from_image` / `print_merge` / `print_match` hooks that let an image
  driver replace NBIS matching (drivers that don't set them are unaffected)
- `libfprint/fprint-list-udev-hwdb.c` and `data/autosuspend.hwdb` — move
  04f3:0c7f from the unsupported-device list to the supported list
- From the earlier NBIS-based matcher, no longer used by this driver's matching:
  - `libfprint/fp-image.c` — tune NBIS parameters for narrow sensors (< 80 px wide)
  - `libfprint/fpi-print.c` — reject scans with too few minutiae; improve debug logging
  - `libfprint/nbis/include/bozorth.h` — lower `MIN_COMPUTABLE_BOZORTH_MINUTIAE` from 10 to 5
  - `libfprint/nbis/mindtct/remove.c` — fix double-free in `remove_malformations`;
    skip `remove_or_adjust_side_minutiae_V2` for narrow images

### 3. Build and install

To build and test:

```
meson setup builddir
ninja -C builddir
meson test -C builddir
```

On Fedora, prefer rebuilding the distribution package over copying the library
into `/usr/lib64` (a later `dnf upgrade` would silently overwrite a copied file):

```
sudo dnf install rpm-build rpmdevtools 'dnf-command(builddep)' 'dnf-command(versionlock)'
sudo dnf builddep libfprint
sudo dnf install opencv-devel
dnf download --source libfprint && rpm -i libfprint-*.src.rpm

# Combined patch (driver + core changes) from the fork branch
git clone -b elan-0c7f-1.94.100 https://github.com/jdtanner/libfprint.git
git -C libfprint fetch https://gitlab.freedesktop.org/libfprint/libfprint.git tag v1.94.100
git -C libfprint diff v1.94.100 HEAD -- meson.build libfprint data \
    > ~/rpmbuild/SOURCES/libfprint-elan0c7f.patch

# In ~/rpmbuild/SPECS/libfprint.spec:
#   Release:  1.1.elan0c7f%{?dist}      (replaces %autorelease)
#   Patch0:   libfprint-elan0c7f.patch  (after Source0)
#   BuildRequires:  opencv-devel
rpmbuild -bb ~/rpmbuild/SPECS/libfprint.spec
sudo dnf install ~/rpmbuild/RPMS/x86_64/libfprint-1.94.100-1.1.elan0c7f.*.x86_64.rpm
sudo dnf versionlock add libfprint
```

On 1.94.100, upstream's `egis_etu905` test fails when the whole suite runs in
parallel, and passes when run alone. Unmodified 1.94.100 fails the same way, so
the failure is not caused by this driver.

---

## GNOME Settings fingerprint dialog fixes

Two bugs in gnome-control-center 49.6 prevent fingerprint enrollment when **no
fingers are enrolled** (first-time setup). These are GNOME bugs, not libfprint
bugs, but were discovered while testing the driver.

Apply with:
```
cd /path/to/gnome-control-center
patch -p1 < /path/to/gnome-control-center.patch
```

### Bug A — UI race condition

`list_enrolled_cb` was gated on `DIALOG_STATE_DEVICE_CLAIMED` before restoring
widget sensitivity. For the zero-enrolled-prints case fprintd returns immediately,
so `list_enrolled_cb` always fires before `claim_device_cb` sets that flag.
Result: the "Scan new fingerprint" button ignored all clicks.

### Bug B — Wayland popup height overflow

With 0 enrolled prints all 10 finger-selection buttons are shown in the popup.
The popup's natural height (~410 px) exceeds the dialog's `content-height: 400`.
On Wayland an `xdg_popup` cannot exceed its parent surface bounds — the compositor
immediately dismisses it, so clicking "Scan new fingerprint" appeared to do nothing.

This patch was written against 49.6 and has not been re-checked against later
releases (Fedora 45 ships 51.1).

Both bugs reported upstream: https://gitlab.gnome.org/GNOME/gnome-control-center/-/issues/3208

---

## Driver design notes

- `FP_DEVICE_FEATURE_IDENTIFY` stays enabled. Since 1.94.100, libfprint
  implements verify for image devices via identify with a single-print gallery,
  so masking the feature makes every verify fail with "Device has not
  identification support". (On 1.94.10 it was masked to work around an fprintd
  crash on identify with an empty gallery during first enrollment.)
- While idle the driver waits on the sensor's own finger detect (`[40 31]`
  `[40 3f]`, then a read on EP83 that completes with `0x55` on touch) instead
  of polling frames, so no USB traffic flows until a finger is placed.
- A `deactivating` flag defers cleanup of `frame_buf` and `best_img` until after
  all in-flight USB transfers complete, avoiding a use-after-free when
  `dev_deactivate` is called while the capture SSM is still running.
- Image quality is scored per frame; the best frame is selected. Frames are
  Gaussian-blurred and 16-bit pixel values are scaled to 8-bit using the per-frame
  min/max range.
- Matching uses SIFT keypoints instead of NBIS minutiae (see below). Prints are
  `FPI_PRINT_RAW` holding a versioned list of keypoint samples, one per
  enrollment scan (12 scans). Prints enrolled by earlier versions of the driver
  (NBIS) no longer match: delete and re-enroll them.
- Debugging: if `ELAN0C7F_DUMP_DIR` is set in fprintd's environment, every
  enroll/verify scan is saved there as a PGM for offline analysis. fprintd's
  systemd sandbox and SELinux policy only let it write under `/var/lib/fprint`,
  so use e.g. `/var/lib/fprint/elan0c7f-dump` (create it with mode 0700 and run
  `restorecon` on it). These are fingerprint images; delete them afterwards.
- **Enroll with natural placement.** Place the finger exactly as you will to
  unlock, every time. The sensor is only ~2.6 mm wide, and deliberately moving
  the finger between enrollment touches gives samples that later verifies
  don't overlap.

---

## Matching

**Status: promising, not yet confirmed.** In the latest live test the enrolled
finger matched 8 times in 10 and no wrong finger was accepted (0 of 10). All
data so far is from one person on one evening; a second session on another day
is still to come before recommending it for login.

### Why not NBIS/Bozorth3

The sensor is 52 px wide, so each scan yields only 3–13 minutiae; Bozorth3 is
designed for 20–40. Measured on 70 captures (30 of the enrolled finger, 40 from
four other fingers), the previous driver settings (threshold 4, any of 8
samples) accepted a wrong finger **55.8%** of the time, and at no setting could
it reject all wrong fingers without also rejecting ~90% of genuine attempts.

### SIFT + RANSAC

Each scan is upscaled ×3, contrast-equalised (CLAHE) and described by SIFT
keypoints (~50–130 per scan). Two scans score the number of descriptor matches
that agree on a single rotation + translation (RANSAC); unrelated fingers'
coincidental matches point in random directions and score low. A verify
matches when any of the 12 enrollment samples scores at least 8.

### The power-key icon

The sensor sits under the power key, which is printed with the power symbol
(⏻). The icon appears in every scan whatever the finger — the average images
of different fingers correlate 0.82–0.94 — so it makes all fingers look alike.
Before extracting features the driver divides each scan by a fixed map of the
icon (`elan0c7f-gain.h`: the blurred average of 40 scans of four fingers that
were never enrolled). The map comes from one unit; other units of the same
model carry the same icon, but its exact position may differ slightly.

### Results

| | Wrong finger accepted | Enrolled finger matched |
|---|---|---|
| Old NBIS settings (offline, 70 captures) | 55.8% | 93% |
| SIFT, no icon correction (live, natural enrollment) | 0 of 5 | 6 of 11 |
| SIFT + icon correction (live, natural enrollment) | **0 of 10** | **8 of 10** |

Replaying saved live scans through the driver's matcher reproduces the live
scores exactly. With the icon correction, an enrollment made with deliberately
shifted placements went from 0 of 11 to 7 of 11 verifies matched. In a
cross-check where the icon map was built from two fingers and wrong-finger
attempts were taken from the other two, none of 60 was accepted (highest score
3–4 against the threshold of 8).

---

## Testing

On libfprint 1.94.100 / Fedora 45:
- `fprintd-enroll` from zero enrolled prints (fprintd's duplicate check runs
  with an empty gallery) — succeeds, no crash
- `fprintd-verify` — see the matching table above

## Tools

`tools/` holds the scripts used for the measurements above:

- `capture.py` — save labelled scans through libfprint (`sudo`, fprintd stopped)
- `score-matrix.c` — score every pair with libfprint's NBIS/Bozorth3
- `sift-score.py` — the SIFT/RANSAC prototype, same output format
- `sift-matrix.c` — score with the driver's own C++ matcher (reproduces
  `sift-score.py` exactly on the measured captures)
- `analyse.py` — genuine vs impostor score distributions and simulated
  FRR/FAR for "K of N enrollment samples score ≥ T"

On libfprint 1.94.10 / Fedora 43 (with IDENTIFY masked):
- `fprintd-enroll` — enrollment succeeds end-to-end
- GNOME Settings → Users → fingerprint enrollment — works from a clean state
  (0 enrolled prints) and with existing prints
- fprintd shutdown after enrollment — no crash
- PAM authentication via fingerprint — works
