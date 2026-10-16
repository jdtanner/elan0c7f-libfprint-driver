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
| `libfprint.patch` | Patch for supporting changes to libfprint core files |
| `gnome-control-center.patch` | Patch for two GNOME Settings fingerprint dialog bugs |

---

## About the sensor

The 04f3:0c7f is an image-based sensor — it captures a raw 52×150 px pixel image
over USB bulk transfer and sends it to the host. All matching is done in software
using libfprint's NBIS/Bozorth3 pipeline. It is **not** a match-on-chip device.

The sensor appears primarily in Acer Swift laptops (SF314-512, SFX14-51G,
SF514-56T, SFX16-52G) and has been probed on over 100 Linux systems, all
previously failing.

---

## Applying the driver

The steps below apply to a libfprint **v1.94.100** source tree.

### 1. Add the driver file

```
cp libfprint/drivers/elan0c7f.c /path/to/libfprint/libfprint/drivers/
```

### 2. Apply the libfprint core patch

```
cd /path/to/libfprint
patch -p1 < /path/to/libfprint.patch
```

This patch covers:
- `libfprint/meson.build` and `meson.build` — register the new driver
- `libfprint/fprint-list-udev-hwdb.c` and `data/autosuspend.hwdb` — move
  04f3:0c7f from the unsupported-device list to the supported list
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
dnf download --source libfprint && rpm -i libfprint-*.src.rpm

# Combined patch (driver + core changes) from the fork branch
git clone -b elan-0c7f-1.94.100 https://github.com/jdtanner/libfprint.git
git -C libfprint fetch https://gitlab.freedesktop.org/libfprint/libfprint.git tag v1.94.100
git -C libfprint diff v1.94.100 HEAD -- meson.build libfprint data \
    > ~/rpmbuild/SOURCES/libfprint-elan0c7f.patch

# In ~/rpmbuild/SPECS/libfprint.spec:
#   Release:  1.1.elan0c7f%{?dist}      (replaces %autorelease)
#   Patch0:   libfprint-elan0c7f.patch  (after Source0)
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

---

## Known issue: false matches

**Matching is not reliable enough for security use.** In testing on
2026-10-16, a different finger was accepted with a score of 7, the same score
the enrolled finger gets. The causes are:

- The sensor is 52 px wide, so each sample yields only 3–11 minutiae. The
  NBIS/Bozorth3 matcher is designed for 20–40.
- To let the genuine finger match at all, the driver sets the Bozorth3 match
  threshold to 4 (upstream libfprint's default is 40).
- A scan is compared against every enrollment sample (8), and any single
  score at or above the threshold is accepted.

Until this is resolved, treat fingerprint unlock on this sensor as a
convenience that another person's finger may be able to pass.

---

## Testing

On libfprint 1.94.100 / Fedora 45:
- `fprintd-enroll` with an existing print enrolled — succeeds
- `fprintd-verify` — matches

On libfprint 1.94.10 / Fedora 43 (with IDENTIFY masked):
- `fprintd-enroll` — enrollment succeeds end-to-end
- GNOME Settings → Users → fingerprint enrollment — works from a clean state
  (0 enrolled prints) and with existing prints
- fprintd shutdown after enrollment — no crash
- PAM authentication via fingerprint — works
