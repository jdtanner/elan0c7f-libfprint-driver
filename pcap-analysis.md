# USB Capture Analysis: ELAN 04f3:0c7f Fingerprint Sensor

**Capture file:** `fpcap.pcapng`
**Duration:** 75.2 seconds, 2270 frames

---

## Methodology

Analysed using `tshark`, extracting USB bulk transfers on the three known endpoints:

- `EP_OUT 0x01` — host to device (commands)
- `EP_IN 0x82` — device to host (frame data)
- `EP_STATUS 0x83` — device to host (status)

---

## Findings

### 1. Raw image data on EP_IN (0x82)

Raw pixel frames are present on EP_IN throughout the entire capture. Every capture
cycle produces exactly **15,600 bytes** of pixel data per frame, split across two
consecutive USB transactions:

```
15,552 bytes  +  48 bytes  =  15,600 bytes
```

This matches the Linux driver's expected frame dimensions exactly:

```
150 rows × 52 cols × 2 bytes/pixel = 15,600 bytes
```

The data is clearly real pixel values — each new finger placement begins with one or
two all-zero frames (sensor settling) followed by frames containing valid 16-bit
little-endian pixel values. A total of **348 complete frames** were captured across
the session.

**The sensor is operating in image-capture mode on Windows, identical to Linux.**

---

### 2. The `[40 ff 01]` command

**`[40 ff 01]` does not appear anywhere in this capture.**

The complete set of EP_OUT commands observed, with frequencies:

| Command | Count | Known purpose |
|---------|-------|---------------|
| `9a 10` | 6 | Init / reset |
| `00 0b` | 4 | Init |
| `40 31` | 23 | Arm sensor |
| `40 3f` | 23 | Wait for finger |
| `00 09` | 348 | Capture frame |

Every host-to-device transfer is exactly **2 bytes**. There are no 3-byte commands
and no commands outside the set already used by the Linux driver.

---

### 3. Large host-to-device transfers (MoC template upload)?

**None.** All 404 EP_OUT transfers are 2 bytes. In a true match-on-chip device,
enrollment requires sending a processed template (typically hundreds to thousands
of bytes) down to the chip for persistent storage. No such transfers occur here.

---

### 4. Enrollment vs verification — protocol differences?

The capture covers both an enrollment session and a verification session. The
boundary between them is a double re-init sequence mid-capture:

```
[enrollment — ~20 arm/capture cycles]

  9a10 → ACK
  000b
  9a10 → ACK        ← session boundary
  000b

[verification — 2 arm/capture cycles]
```

**Both sessions are protocol-identical.** In each cycle:

```
write [40 31]              -- arm sensor
write [40 3f] → 0x55       -- finger detected
write [00 09] → 15600 bytes -- capture frame
write [00 09] → 15600 bytes
... (repeat until finger lifted, zero frame returned)
```

The host collects raw frames in both enrollment and verification and performs all
matching itself. There is no structural difference between the two sessions at the
USB level.

---

### 5. EP_STATUS (0x83) responses

Only two distinct responses were observed:

| Response | Count | Meaning |
|----------|-------|---------|
| `55` | 23 | Finger present (returned after `40 3f`) |
| `40 9a 10 00` | 6 | Echo/ACK of `9a 10` init command |

No structured match results, template IDs, or score values were returned by the
sensor at any point.

---

## Summary

| Question | Answer |
|----------|--------|
| Image-capture mode on Windows? | **Yes** — 15,600-byte raw frames on every capture |
| `[40 ff 01]` present in capture? | **No** |
| Large host→device transfers? | **No** — all commands are 2 bytes |
| Evidence of match-on-chip operation? | **None** |
| Windows protocol differs from Linux driver? | **No** — command set is identical |

---

## Conclusions

This capture provides no evidence that the 04f3:0c7f sensor supports or uses
match-on-chip on Windows. The Windows driver uses the same five-command protocol as
the Linux driver, streaming raw 52×150 pixel frames to the host for all processing.

The `[40 ff 01]` command noted in the briefing as a potential MoC enroll trigger
is absent from this capture. If it was observed previously, it was either in a
different capture or inferred from static analysis of the driver binary rather than
a live trace.

**Recommendation:** There is no basis from this capture to pursue MoC support for
the 0c7f. The device behaves as a pure image sensor under both the Windows and Linux
drivers. If a separate capture containing `[40 ff 01]` exists, that should be
examined before drawing further conclusions.
