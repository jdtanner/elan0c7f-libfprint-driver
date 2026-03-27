# ELAN 04f3:0c7f Fingerprint Reader — libfprint Driver

libfprint driver for the **ELAN Microelectronics 04f3:0c7f** fingerprint sensor,
found in Acer Swift laptops. Tested on Fedora 43, Wayland, with fprintd 1.94.5.

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
- `libfprint/fp-image.c` — tune NBIS parameters for narrow sensors (< 80 px wide)
- `libfprint/fpi-print.c` — reject scans with too few minutiae; improve debug logging
- `libfprint/nbis/include/bozorth.h` — lower `MIN_COMPUTABLE_BOZORTH_MINUTIAE` from 10 to 5
- `libfprint/nbis/mindtct/remove.c` — fix double-free in `remove_malformations`;
  skip `remove_or_adjust_side_minutiae_V2` for narrow images

### 3. Build and install

```
meson setup builddir
ninja -C builddir
sudo cp builddir/libfprint/libfprint-2.so.2.0.0 /usr/lib64/libfprint-2.so.2.0.0
```

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

Both bugs reported upstream: https://gitlab.gnome.org/GNOME/gnome-control-center/-/issues/3208

---

## Driver design notes

- `FP_DEVICE_FEATURE_IDENTIFY` is disabled — the firmware does not support 1:N
  identification. Leaving it enabled caused fprintd to crash during first enrollment.
- A `deactivating` flag defers cleanup of `frame_buf` and `best_img` until after
  all in-flight USB transfers complete, avoiding a use-after-free when
  `dev_deactivate` is called while the capture SSM is still running.
- Image quality is scored per frame; the best frame is selected. Frames are
  Gaussian-blurred and 16-bit pixel values are scaled to 8-bit using the per-frame
  min/max range.

---

## Testing

- `fprintd-enroll` — enrollment succeeds end-to-end
- GNOME Settings → Users → fingerprint enrollment — works from a clean state
  (0 enrolled prints) and with existing prints
- fprintd shutdown after enrollment — no crash
- PAM authentication via fingerprint — works
