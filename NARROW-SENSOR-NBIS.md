# NBIS Fixes for Narrow Fingerprint Sensors

This document explains the changes made to libfprint's NBIS pipeline to support
fingerprint sensors that produce narrow images. The ELAN 04f3:0c7f produces images
52 pixels wide by 150 pixels tall — far narrower than the sensors NBIS was
designed and tuned for.

---

## Background

libfprint uses NBIS (NIST Biometric Image Software) for fingerprint feature
extraction and Bozorth3 for matching. NBIS was developed by the US National
Institute of Standards and Technology and is tuned for sensors producing images
in the range of 150–500 pixels wide. Its default parameters reflect assumptions
about image geometry that break down badly on narrow sensors.

The 0c7f sensor produces 52×150 px images. Five separate problems were identified
and fixed.

---

## Problem 1 — Edge margins consuming most of the image

**File:** `libfprint/fp-image.c`  
**Parameter:** `inv_block_margin`, `trans_dir_pix`

NBIS divides the image into blocks and applies an inversion margin — a border
region near the image edge that is excluded from analysis on the assumption that
ridge data close to the edge is unreliable. The default margin is 4 pixels on
each side.

On a 52 pixel wide image, 4 pixels of margin on each side consumes 15% of the
image width before any analysis begins. Combined with block sizing effects, this
left very little usable image area.

**Fix:** When image width is less than 80 px, reduce `inv_block_margin` and
`trans_dir_pix` to 1. This preserves the edge-exclusion concept while recovering
most of the image for analysis.

---

## Problem 2 — Side minutiae removal discarding valid features

**Files:** `libfprint/fp-image.c`, `libfprint/nbis/mindtct/remove.c`  
**Parameter:** `side_half_contour`  
**Function:** `remove_or_adjust_side_minutiae_V2`

NBIS has a routine that removes or adjusts minutiae (ridge endings and
bifurcations) that are on the side of a ridge rather than at its tip. It works
by tracing a contour along the ridge for `side_half_contour` pixels in each
direction from the minutia and checking the geometry.

On a narrow 52 px wide image, ridges run very close to the image edge. When
NBIS traces a contour outward from a minutia near the edge, the trace immediately
runs off the image boundary and returns an IGNORE result, which the removal
routine interprets as a reason to discard the minutia.

The result: almost every minutia near the left or right edge of the image —
which on a 52 px wide image means almost every minutia — was being discarded as
suspicious. The sensor was producing valid fingerprint data but NBIS was throwing
it away.

**Fix:** Set `side_half_contour` to 0 for images narrower than 80 px, and add a
guard in `remove_or_adjust_side_minutiae_V2` to skip the routine entirely when
`side_half_contour == 0`. This is safe because the side minutiae check is a
quality filter, not a correctness requirement — skipping it means keeping more
minutiae, some of which may be marginal, but this is far preferable to discarding
everything.

---

## Problem 3 — Double-free crash in remove_malformations

**File:** `libfprint/nbis/mindtct/remove.c`  
**Function:** `remove_malformations`

This is a latent bug in NBIS that rarely triggers on wide-sensor images but
hit consistently on narrow images where the contour trace frequently returns
IGNORE.

The `remove_malformations` function loops over minutiae and calls
`trace_contour` twice per iteration. The contour pointers (`contour_x`,
`contour_y`, `contour_ex`, `contour_ey`) are allocated by `trace_contour`
when it succeeds and must be freed by the caller. When `trace_contour`
returns IGNORE it does not allocate anything, leaving the pointers
unchanged from whatever they held previously.

The bug: if the first `trace_contour` call succeeds (allocating memory),
the pointers are freed at the end of that code path. But if on the next
loop iteration `trace_contour` immediately returns IGNORE, the pointers
still hold the addresses of the memory freed in the previous iteration.
A subsequent `free_contour` call on those stale pointers is a double-free,
causing a crash.

On wide images contour traces rarely return IGNORE so the pointers are
almost always overwritten before being freed again. On narrow images IGNORE
is frequent, making the double-free almost certain.

**Fix:** Reset all four contour pointers and `ncontour` to NULL/0 at the
top of each loop iteration, and again immediately after each `free_contour`
call. This ensures a stale pointer from a previous iteration can never be
freed again.

---

## Problem 4 — Minimum minutiae threshold too high for matching

**File:** `libfprint/nbis/include/bozorth.h`  
**Parameter:** `MIN_COMPUTABLE_BOZORTH_MINUTIAE`

Bozorth3 (the matching algorithm) has a minimum minutiae count below which it
will not attempt a match at all, on the basis that too few minutiae cannot
produce a meaningful score. This was set to 10.

After the fixes above, the 0c7f sensor consistently produces 5–8 good minutiae
per scan. This is enough for Bozorth3 to produce a reliable match score — the
algorithm is designed to work with as few as 5 minutiae — but the hard floor of
10 was causing every scan to be rejected before matching was even attempted.

**Fix:** Lower `MIN_COMPUTABLE_BOZORTH_MINUTIAE` from 10 to 5, which is the
genuine lower bound of what Bozorth3 can compute a score from.

---

## Problem 5 — No early rejection for scans with too few minutiae

**File:** `libfprint/fpi-print.c`  
**Function:** `fpi_print_add_from_image`

Without an early rejection path, scans that produced very few minutiae (fewer
than 3) would be recorded as templates and submitted for matching, producing
scores of zero and causing enrollment to fail silently or produce an unusable
template.

**Fix:** After converting minutiae to the xyt structure used by Bozorth3, reject
the scan immediately if `nrows < 3` with a `FP_DEVICE_RETRY_CENTER_FINGER` error.
This tells libfprint to retry the capture stage rather than treating it as a hard
failure, prompting the user to reposition their finger.

The threshold of 3 rather than 5 is intentional: nrows of 3–4 can occasionally
produce a marginal match score, and blocking at 5 caused almost every scan of
this narrow sensor to trigger the retry, making enrollment frustrating. Three is
the practical floor below which a match score of > 0 is essentially impossible.

---

## Summary of parameter changes

| Parameter | Original | New | File |
|-----------|----------|-----|------|
| `inv_block_margin` | 4 | 1 | `fp-image.c` (narrow only) |
| `trans_dir_pix` | 4 | 1 | `fp-image.c` (narrow only) |
| `side_half_contour` | 7 | 0 | `fp-image.c` (narrow only) |
| `max_hook_len` | 15 | 10 | `fp-image.c` (narrow only) |
| `MIN_COMPUTABLE_BOZORTH_MINUTIAE` | 10 | 5 | `bozorth.h` (global) |

All parameter changes except `MIN_COMPUTABLE_BOZORTH_MINUTIAE` are conditional
on `image->width < 80` and do not affect existing drivers or wide sensors.

