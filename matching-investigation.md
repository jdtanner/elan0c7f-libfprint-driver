# Port to libfprint 1.94.100 and Matching Investigation

**Date:** 2026-10-16
**System:** Fedora 45, libfprint 1.94.100 (Fedora package rebuilt with this driver), fprintd 1.94.5
**Result:** the driver now matches with SIFT keypoints after removing the
power-key icon from each scan, and (since 2026-11-01, section 8) learns from
confident matches. In the latest live test the enrolled finger matched 8 times
in 10 and no wrong finger was accepted (0 of 10).

This document records what was changed, what was measured, and what is still open.

---

## 1. Port to libfprint 1.94.100

The driver had been developed against libfprint 1.94.10. Fedora 45 ships 1.94.100,
so the driver commits were moved onto upstream `v1.94.100` (branch
`elan-0c7f-1.94.100` of https://github.com/jdtanner/libfprint).

- **Merge conflicts:** upstream renamed `template` to `print_template` in
  `fpi-print.c`, and replaced the driver lists in both `meson.build` files with a
  `drivers_info` dictionary. Both were resolved by hand.
- **Unsupported-device list:** 04f3:0c7f was still listed in upstream's
  unsupported-device list (`fprint-list-udev-hwdb.c`); it was removed and
  `data/autosuspend.hwdb` regenerated.
- **Verify was broken:** upstream 1.94.100 removed the separate verify path for
  image devices (`bebaa80c`) and now verifies via identify with a one-print
  gallery (`7fee9288`). The driver masked out `FP_DEVICE_FEATURE_IDENTIFY` (to
  avoid an fprintd crash on 1.94.10), so every verify failed immediately with
  "Device has not identification support". IDENTIFY is enabled again. Enrolling
  from zero prints (the old crash case) was tested and works.
- **Upstream test failure:** `egis_etu905` fails when the whole test suite runs in
  parallel and passes alone. Unmodified 1.94.100 fails the same way, so it is not
  caused by this driver.

### Installation

Rather than copying the library over `/usr/lib64` (which the next `dnf upgrade`
silently overwrites), the Fedora `libfprint` source package is rebuilt with the
fork's diff as a patch, installed as `libfprint-1.94.100-1.N.elan0c7f`, and held
with `dnf versionlock`. The README describes the steps.

---

## 2. Code review fixes

- **Incomplete published patch:** `libfprint.patch` in this repository held only
  the driver and meson registration. The NBIS changes the README described were
  missing, and because the patch also created `elan0c7f.c`, the README's
  `cp` + `patch` steps failed. It is now regenerated from the fork, and following
  the README on a clean 1.94.100 tree is checked to reproduce the fork exactly.
- **Leaks:** SSM completion callbacks own the `GError` they receive; the
  deactivating paths of `capture_done` and `reinit_done` leaked it. `frame_buf`
  leaked when activation failed.
- **Comment accuracy:** the protocol comment described reading `0x55` from EP83,
  which the driver did not do.
- **Copyright holder** corrected to John Hill-Tanner.
- **Code style:** the driver now passes upstream's `uncrustify` check.

### fprintd

fprintd has had no release since 1.94.5. The unreleased commits on its main
branch mainly concern match-on-chip devices with on-sensor storage, plus
utility and test changes; none require driver changes.

---

## 3. Waiting for a finger without polling

While waiting for a finger, the driver streamed full frames continuously.
Measured with usbmon: **113 frame transfers in 5 seconds** (~23 frames/s,
~350 KB/s), which also stopped the USB device from ever suspending.

The sensor has its own finger detect: after `[40 31] [40 3f]`, a read on EP83
completes with `0x55` only when a finger is placed (`0xca` = busy, re-arm). The
capture cycle now arms the sensor and blocks on that read, with a `GCancellable`
that `dev_deactivate` cancels. After 5 unusable frames in a row it re-arms
rather than polling.

Measured afterwards: **no USB transfers** while waiting, and a cancelled verify
deactivates within 1 ms.

---

## 4. False matches with NBIS/Bozorth3

During testing a **wrong finger was accepted** with a Bozorth3 score of 7, the
same score the enrolled finger got.

### Measurement

To measure rather than guess, scans were collected with `tools/capture.py`
(30 of the enrolled right index finger, 10 each of four other fingers) and scored
offline with libfprint's own minutiae extraction and Bozorth3
(`tools/score-matrix.c`). `tools/analyse.py` simulates fprintd's decision: a probe
matches when at least K of the N enrollment samples score at least T.

| Bozorth3 setting | Wrong finger accepted | Enrolled finger rejected |
|---|---|---|
| Driver's setting (T=4, K=1, 8 samples) | **55.8%** | 6.7% |
| T=10, K=1 | 14.0% | 18.6% |
| T=15, K=2 | 1.2% | 58.1% |
| Lowest setting with no wrong finger accepted | 0% | 86–95% |

The sensor is 52 px wide, so each scan yields only 3–13 minutiae; Bozorth3 needs
20–40 to tell fingers apart. No threshold works: wrong-finger scores reach 27,
while the enrolled finger's best is 29.

Fingerprint login was disabled on the test machine at this point.

---

## 5. SIFT + RANSAC matcher

An image-texture matcher, in the spirit of the SIGFM approach some libfprint forks
use for small sensors, was prototyped first in Python (`tools/sift-score.py`):

1. upscale the scan ×3 and equalise contrast (CLAHE);
2. find SIFT keypoints (~50–130 per scan);
3. pair descriptors between two scans, keeping pairs that pass Lowe's ratio test;
4. score = the number of pairs consistent with one rotation + translation (RANSAC).

Coincidental matches between different fingers point in random directions, so they
score low.

Offline on the same scans, wrong-finger pairs never scored above 6, while the
enrolled finger scored up to 79. At the strictest setting with no wrong finger
accepted, 17% of genuine attempts were rejected (8 enrollment samples), against
95% for Bozorth3.

### In the driver

- **Core hook:** `FpImageDeviceClass` gains optional `print_from_image`,
  `print_merge` and `print_match` vfuncs. When a driver sets them, enrollment prints
  are `FPI_PRINT_RAW` and the driver builds and matches its own data; other drivers
  are unaffected.
- **Matcher:** `elan0c7f-sift.cpp` reimplements the prototype with OpenCV.
  `tools/sift-matrix.c` checks it reproduces the prototype exactly (all 4,830
  pair scores identical).
- **Prints:** a versioned list of keypoint samples, one per enrollment scan.
  Older prints are reported as non-matching rather than as errors, so fprintd's
  duplicate check does not block re-enrolling.
- **Settings:** match threshold 8; 12 enrollment scans instead of 8.
- **Dependency:** the driver needs OpenCV (it links only core, imgproc,
  features2d, flann and calib3d).

---

## 6. Live results lower than offline, and why

The first live tests matched the enrolled finger far less often than predicted:
6 of 11, then 2 of 16, then 0 of 11. To see why, the driver gained a debug option
(`ELAN0C7F_DUMP_DIR`) that saves every enroll and verify scan. fprintd's systemd
sandbox and SELinux policy only allow it to write under `/var/lib/fprint`, so the
directory must live there.

Replaying the saved scans offline reproduced the live scores exactly, so the
matcher itself behaved as measured. The problem was in the scans.

### Enrollment placement

The 12 enrollment scans from the 0-of-11 session barely matched *each other*
(mean score 0.9, about the same as different fingers). Enrollment instructions had
asked for the finger to be shifted between touches; on a sensor about 2.6 mm wide,
that spreads the samples over patches a natural verify placement doesn't overlap.
The same verify scans matched 5–8 times in 11 against an enrollment made with
consistent placement. **Enroll with the placement you will use to unlock.**

### The power-key icon

Averaging scans showed a pattern common to every finger: the average images of
different fingers correlate 0.82–0.94, and a single scan of one finger correlates
0.74 with the average of the other fingers. The pattern is the **power symbol (⏻)
printed on the power key** the sensor sits under. It shows rotated a quarter turn,
because the driver uses the sensor image in portrait orientation.

The driver now divides each scan by a fixed map of the icon before extracting
features (`elan0c7f-gain.h`: the blurred average of 40 scans of four fingers that
were never enrolled). In a cross-check that built the map from two fingers and
took wrong-finger attempts from the other two, none of 60 was accepted. On saved
live sessions:

| Session | Before | With icon correction |
|---|---|---|
| Natural-placement enrollment | 6 of 11 | 8 of 11 |
| Shifted-placement enrollment | 0 of 11 | 7 of 11 |
| Capture-tool scans, later 18 vs first 12 | 14 of 18 | 18 of 18 |
| Wrong fingers vs these enrollments | 0 of 120 | 0 of 120 |

The print format was bumped to version 2; prints made before the correction must
be re-enrolled.

---

## 7. Live results

| Version | Enrolled finger matched | Wrong finger accepted |
|---|---|---|
| Original driver (NBIS, threshold 4) | 2 of 2 | **1 of 2** (score 7) |
| SIFT matcher | 6 of 11, 2 of 16, 0 of 11 (three enrollments) | 0 of 5 |
| SIFT + icon correction (current) | **8 of 10** | **0 of 10** (highest score 3, threshold 8) |

Fingerprint login was re-enabled on the test machine after the last result.

---

## 8. Second session (2026-11-01): the enrollment, then learning

Two weeks of daily use: fingerprint unlock "doesn't work a lot, but never seems to
false positive". The journal held no scores (debug logging had been switched off
after 16 Oct), so the driver's debug log and scan saving were switched on for
fprintd with a systemd drop-in:

```
# /etc/systemd/system/fprintd.service.d/elan0c7f-debug.conf
[Service]
Environment=G_MESSAGES_DEBUG=libfprint-elan0c7f
Environment=ELAN0C7F_DUMP_DIR=/var/lib/fprint/elan0c7f-dumps
```

Two journal messages that look like faults and are not:

- "Driver reported a match providing a scanned print that is not matching any in
  the gallery" is printed on every **successful** match: libfprint compares the
  scanned probe against the gallery with `fp_print_equal`, exempts only NBIS
  prints, and on failure drops the probe copy while keeping the match. It counts
  successes (20 between 17 Oct and 1 Nov).
- "Cannot run while suspended" is the lock screen's verify being cut off as the
  laptop goes to sleep.

### The enrollment was the fault

The first 10 live touches unlocked once. None was a near miss: the nine failures
scored 0 to 5, the range a wrong finger scores. Replaying them (the replay
reproduced every live score) showed why:

- The print in use, enrolled on 24 Oct, had samples that barely matched each other
  (best score per sample 0 to 14, eight of the twelve at 4 or below): the shifted
  placement pattern of section 6.
- The same touches matched **each other** strongly (seven of ten scored 8 to 32
  together) and matched the 16 Oct scans of the same finger (up to 30). Placement
  was consistent; the enrollment did not cover it.

Re-enrolling with natural placement gave samples scoring 12 to 52 against each
other. Replayed against it, 7 of the same 10 touches would have unlocked, and the
next 10 fresh touches unlocked 8 times, both misses followed by a success on the
next touch.

**Natural wobble is wanted in an enrollment; deliberate shifting is not.** Twelve
touches placed the way you unlock vary by themselves, and that variation is what
the samples are for.

### Learning

Twelve scans of a 2.6 mm wide patch can only cover part of a finger, and the
Windows engine adapter's settings (`MaxEnrollNum` beside a separate
`MaxTemplateNum`) hint that it keeps adding touches after enrollment. fprintd
saves a print only when enrolling, and libfprint's `FP_DEVICE_FEATURE_UPDATE_PRINT`
only adds enrollment stages to an existing print, so learning lives in the driver
(`elan0c7f-learn.c`):

- A touch scoring at least **15** against an **enrolled** sample is kept. A match
  against a learned sample unlocks but is never learned from, so the print cannot
  drift one slightly-off touch at a time.
- At most **24** learned samples, newest kept, so changing skin replaces itself.
- One file per enrollment in `/var/lib/fprint/elan0c7f-learned/`, named by the
  SHA-256 of the enrolled samples, mode 0600 in a 0700 directory. Re-enrolling
  gives a new name and starts afresh. `ELAN0C7F_LEARN_DIR` overrides the
  directory; set to an empty string it turns learning off.

Chosen by replaying 63 real scans of the enrolled finger in the order they were
made (the 1 Nov touches, then the 16 Oct scans as a stand-in for another day),
against 40 scans of four other fingers (`tools/learn-sim.py`):

| Setting | Unlocked | Highest wrong-finger score (8 unlocks) |
|---|---|---|
| No learning | 38 of 63 | 4 |
| Learn at 20 or more, keep 24 | 45 of 63 | 4 |
| **Learn at 15 or more, keep 24** | **56 of 63** | 5 |
| Learn at 15 or more, keep 12 | 55 of 63 | 5 |
| Learn at 12 or more, keep 24 | 56 of 63 | 5 |

Most of the gain is on the 16 Oct scans, the different-day case. The wrong-finger
margin narrowed by one point at the chosen setting, which is the thing to keep
watching. Installed as `libfprint-1.94.100-1.8.elan0c7f`; first live use learned
a touch scoring 17 and wrote the file with the expected mode and SELinux label.

**Known gap:** deleting a fingerprint through fprintd does not delete its learned
file, since the driver is not told about deletions. The file is unreachable once
its print is gone (nothing else has that hash), but it is still fingerprint data:
`sudo rm -r /var/lib/fprint/elan0c7f-learned` after deleting prints.

---

## 9. Open items

- **More days of data:** two evenings from one person so far. Keep the debug
  drop-in on for a week or two and re-run `tools/learn-sim.py` on the saved scans,
  watching the highest wrong-finger score as learned samples accumulate.
- **Other units:** the icon map was built on one laptop. Other units of the same
  model carry the same icon, but its exact position may differ slightly; this is
  untested.
- **Windows calibration:** the Windows driver runs a gain/bias calibration at open
  (see `driver-analysis.md`). It is not implemented, and would not remove the
  printed icon, but might still improve image quality.
- **Upstreaming:** the OpenCV dependency is unlikely to be accepted by upstream
  libfprint. The NBIS-related core changes (`bozorth.h`, `fpi-print.c`) are no
  longer used by this driver's matching and could be dropped.

---

## Tools

| Tool | Purpose |
|---|---|
| `tools/capture.py` | Save labelled scans through libfprint (`sudo`, fprintd stopped) |
| `tools/score-matrix.c` | Score every scan pair with libfprint's NBIS/Bozorth3 |
| `tools/sift-score.py` | The SIFT/RANSAC prototype |
| `tools/sift-matrix.c` | Score with the driver's own C++ matcher |
| `tools/analyse.py` | Genuine vs impostor distributions and simulated FRR/FAR |
| `tools/learn-sim.py` | Replay scans in order with and without learning; the driver's matcher in Python |
| `tools/learn-test.c` | Unit tests for the learned-sample store (`elan0c7f-learn.c`) |

Scan data is not included in this repository: fingerprint images are biometric data.
