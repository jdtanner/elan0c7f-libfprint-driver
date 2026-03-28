# Reverse Engineering: Enroll / Verify / Capture Functions

**Files:** `WbfUsbDriver.dll`, `EngineAdapter.dll`
**Method:** PE export resolution, interface table parsing, `objdump` disassembly

---

## Architecture overview

The Windows Biometric Framework (WBF) splits fingerprint work across two
adapter DLLs:

```
WBF Service
    │
    ├── WbfUsbDriver.dll   (Sensor Adapter — USB I/O, image capture)
    │       export: FxDriverEntryUm  → registers WDF callbacks
    │
    └── EngineAdapter.dll  (Engine Adapter — feature extraction, matching)
            export: WbioQueryEngineInterface  → returns function pointer table
```

The engine adapter fills a `WINBIO_ENGINE_INTERFACE` structure (version 3,
41 entries, 360 bytes). All matching and template storage runs entirely on
the host. There is no on-chip component.

---

## Engine Adapter function table (EngineAdapter.dll)

Table located at VA `0x180053030`. All addresses are in `EngineAdapter.dll`.

| # | Function | VA | Notes |
|---|----------|----|-------|
| 0 | Attach | `0x180001720` | Initialises pipeline callbacks |
| 1 | Detach | `0x180001aa0` | |
| 2 | ClearContext | `0x180001b60` | |
| 3 | QueryPreferredFormat | `0x180001c30` | |
| 4 | QueryIndexVectorSize | `0x180001c60` | |
| 5 | QueryHashAlgorithms | `0x180001c80` | |
| 6 | SetHashAlgorithm | `0x180001cc0` | |
| 7 | QuerySampleHint | `0x180001ce0` | |
| **8** | **AcceptSampleData** | **`0x180001d20`** | Image processing — see below |
| 9 | ExportEngineData | `0x1800024a0` | |
| **10** | **VerifyFeatureSet** | **`0x1800024d0`** | 1:1 match — see below |
| **11** | **IdentifyFeatureSet** | **`0x180002b70`** | 1:N search — see below |
| **12** | **CreateEnrollment** | **`0x1800036d0`** | Begin enrol session |
| **13** | **UpdateEnrollment** | **`0x1800037c0`** | Add one sample |
| **14** | **GetEnrollmentStatus** | **`0x1800038b0`** | Progress / done? |
| 15 | GetEnrollmentHash | `0x180003900` | |
| 16 | MergeEnrollment | `0x180003910` | Merge multi-sample template |
| **17** | **CommitEnrollment** | **`0x180003e30`** | Write template to store |
| **18** | **DiscardEnrollment** | **`0x180004070`** | Abort enrolment |
| 19 | ControlUnit | `0x180004140` | Vendor IOCTL pass-through |
| 20 | ControlUnitPrivileged | `0x180004410` | |
| 21 | NotifyPowerChange | `0x180004420` | |
| 22 | (Reserved) | NULL | |
| 23 | PipelineInit | `0x180004450` | |
| 24 | PipelineCleanup | `0x180004470` | |
| 25 | Activate | `0x180004620` | |
| 26 | Deactivate | `0x180004770` | |
| 27 | QueryExtendedInfo | `0x180004980` | |
| 28 | IdentifyAll | `0x180004a70` | |
| 29–40 | (calibration / DB) | various | Not implemented (NULL) |

---

## AcceptSampleData (`0x180001d20`)

This is the image processing pipeline. It receives a raw 52×150 pixel
16-bit frame and produces a 76-byte feature vector.

**Key observations:**
- Stack frame is **0x78e0 bytes** (≈ 31 KB) — heavy image processing
- Validates sample format: type `0x1b`, subtype `0x401`
- Calls six image-processing sub-functions:
  - `0x180008770` — primary feature extraction
  - `0x1800088c0` — second processing pass
  - `0x180008660` — quality assessment
- Checks a "high-quality mode" global flag at `0x1800671f0`:
  - If set: returns immediately with status `0x1` or `0x2` (need more)
    and does not run feature extraction (calibration/warmup mode)
  - If clear: runs full extraction and produces feature vector
- HRESULT codes checked: `0x8009801f` (bad capture), `0x80098005` (no
  match), `0x80070057` (invalid arg)
- String evidence of processing stages in the binary:
  `_LowPass_filter_replicate_for_Mask`, `ImageGetMask`, `ImageGetWaterMask`,
  `Image_Processing_coating`, `Image_Processing_glass`,
  `Check_ACNoise_RowReplace`, `ImageACProcessing_Single`, `AC_Detect`

---

## Enrollment flow

```
CreateEnrollment()
    ├── Clears enrollment context
    ├── Checks/sets high-quality mode global (0x1800671f0)
    └── Calls storage pipeline with code 0x44221c to open enrollment slot

UpdateEnrollment(context, sample)              ← called per captured frame
    ├── Validates enrollment is active ([ctx+0x10] == 1)
    ├── IF high-quality mode:
    │     increments counter [ctx+0x30]
    │     returns 0 immediately (no feature extraction, warmup frames)
    └── ELSE:
          calls feature extraction at 0x1800082c0
          increments counter [ctx+0x30]

GetEnrollmentStatus(context) → status
    ├── Reads capture count [ctx+0x34]
    ├── Returns 0x90001 (WINBIO_E_ENROLLMENT_IN_PROGRESS) if count < threshold
    ├── IF high-quality mode (0x1800671f0 == 1):
    │     threshold = 0x31 (49 samples needed)
    └── Returns 0 (complete) when count ≥ threshold

CommitEnrollment(context, identity, fingerIndex, payloadBlob)
    ├── Validates enrollment is complete ([ctx+0x10] == 1, count ≥ threshold)
    ├── Calls host-side template merging (MergeEnrollment at 0x180003910)
    ├── Writes template to WBF storage via pipeline callback (code 0x44221c)
    └── Clears enrollment context
```

**Enrollment requires up to 49 captured samples** before `GetEnrollmentStatus`
returns complete. This explains why the pcap shows ~20 arm/capture cycles per
session — each cycle supplies multiple frames, and the driver accumulates them
until 49 good samples are obtained.

---

## Template structure

Each stored template entry is **104 bytes (0x68)** in an in-memory array at
global `0x18006d788`.

| Offset | Size | Content |
|--------|------|---------|
| 0 | 1 | Fingerprint/user index (compared during lookup) |
| 1 | 3 | (padding / flags) |
| 4 | 76 (0x4c) | Feature vector (76 bytes) |
| 80 | 8 | Pointer to heap-allocated extended data |
| 88 | 16 | Remainder (counts, flags) |

The feature vector is 76 bytes. Template storage is held in an in-memory
array; persistence is handled by the WBF pipeline storage callbacks.

---

## VerifyFeatureSet (`0x1800024d0`) — 1:1 match

```
VerifyFeatureSet(context, sampleBuffer, rejectDetail,
                 identity, fingerIndex, accuracy,
                 payloadBlob, rejectDetail2)

1. Calls AcceptSampleData to get current 76-byte feature vector
2. Calls WBF pipeline (cb 0x44221c) to load stored template
3. Calls 0x1800064b0 (template search):
   a. Iterates stored template array at 0x18006d788
   b. For each entry: checks fingerprint index matches
   c. Calls memcmp(stored[4..79], current_feature[0..75], 76)
   d. Returns index on exact match, -1 on no match
4. If match found: sets WINBIO_REJECT_DETAIL = 0 (success)
5. If no match: returns WINBIO_E_NO_MATCH (0x80098005)
```

**Note:** The comparison uses byte-exact memcmp on the 76-byte vector. This
implies the feature extraction algorithm is deterministic and quantised — for
a matching finger, the algorithm always produces the same 76-byte output.
This is a correlation or Fourier-based approach rather than minutiae matching.

---

## IdentifyFeatureSet (`0x180002b70`) — 1:N search

Same as VerifyFeatureSet but iterates over **all stored templates** regardless
of fingerprint index, returning the best match identity and finger index.

---

## Sensor Adapter summary (WbfUsbDriver.dll)

`WbfUsbDriver.dll` has no named exports beyond `FxDriverEntryUm` (the WDF
entry point). The sensor adapter functions are registered as WDF I/O request
callbacks rather than exported. From the disassembly of the USB command sites:

| Sensor operation | USB sequence |
|-----------------|--------------|
| Open / attach | `[9a 10]` × 3, `[00 0b]` × 2 (init) |
| Calibrate | `[40 7d]`, `[40 bd XX]`, `[40 a8 78]`, poll `[40 68]`, then config writes |
| StartCapture | `[40 31]` (arm), `[40 3f]` → 0x55 (wait for finger) |
| CaptureFrame | `[00 09]` → 15,600 bytes on EP_IN (0x82) per frame |
| CancelCapture | `[40 3e]` → checks response for 0xaa (done) / 0xff (stop) |
| Close | `[9a 10]`, `[00 0b]` (re-init / teardown) |

The sensor adapter accumulates raw frames from `[00 09]` and pushes them to
the engine adapter via WBF `PushDataToEngine`. The engine adapter's
`AcceptSampleData` then processes each frame.

---

## Summary for Linux driver development

The complete host-side pipeline maps cleanly to libfprint's existing model:

| Windows component | libfprint equivalent |
|-------------------|----------------------|
| `AcceptSampleData` + image processing | libfprint NBIS pipeline |
| 76-byte feature vector | libfprint minutiae / feature set |
| 49-sample enrollment | libfprint multi-sample enrollment |
| `VerifyFeatureSet` memcmp loop | libfprint identify/verify |
| WBF pipeline storage callbacks | libfprint template storage |

The existing libfprint `elanspi` driver approach (capture raw frames, pass
to NBIS on the host) is **exactly correct** for this device. No changes to
the capture protocol are needed. The only difference between the Windows
and Linux drivers is:

1. Windows runs a calibration sequence at open time (`[40 7d]`, `[40 bd]`,
   `[40 a8]` etc.) that the Linux driver does not currently send.
2. Windows collects up to 49 frames per enrollment session; the Linux driver
   may use a different count.

These calibration commands may be worth adding to the Linux driver to improve
image quality, but are not required for correct operation.
