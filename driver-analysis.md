# Windows Driver Binary Analysis: ELAN 04f3:0c7f

**Files analysed:**
- `WbfUsbDriver.dll` — USB driver (Windows Biometric Framework UMDF)
- `EngineAdapter.dll` — biometric engine adapter
- `ELANFPService.exe` — ELAN fingerprint service

**Tools used:** `strings`, `objdump` (PE/x86-64 disassembly)

---

## Project context

The PDB debug path embedded in `WbfUsbDriver.dll` is:

```
D:\CS_Fingerprint_Acer_UnderGlass\CB64\WBF_UMDF\WbfUsbDriver\
```

This driver is built as a User-Mode Driver Framework (UMDF) component for the
Windows Biometric Framework. The `UnderGlass` name in the path likely refers to
the sensor family (under-glass cover, e.g. under a touchpad surface).

---

## Key finding: no MoC commands in any binary

A raw byte search across all three binaries for the elanmoc command signatures
found nothing:

| Pattern | WbfUsbDriver.dll | EngineAdapter.dll | ELANFPService.exe |
|---------|-----------------|-------------------|-------------------|
| `40 ff 01` (enroll) | **absent** | **absent** | **absent** |
| `40 ff 03` (verify) | **absent** | **absent** | **absent** |
| `40 ff 13` (delete) | **absent** | **absent** | **absent** |

`[40 ff 01]` is not present as a byte sequence anywhere in this driver package.
The driver does not support match-on-chip.

---

## Command architecture

`WbfUsbDriver.dll` contains two small command-builder functions:

### 2-byte command builder (at `0x18000413c`)

```
buf[0] = 0x40
buf[1] = parameter + 0x40   // e.g. pass 0xf7 to get second byte 0x37
write(EP_OUT, buf, 2)
read(EP_IN or EP_STATUS, response, 2)
```

### 3-byte command builder (at `0x1800041c8`)

```
buf[0] = 0x40
buf[1] = parameter + 0x80   // e.g. pass 0x07 to get second byte 0x87
buf[2] = third_byte
write(EP_OUT, buf, 3)
// no response read — fire and forget
```

All EP_OUT transfers pass through one of these two builders. Every command sent
to the device is 2 or 3 bytes. There are no large host-to-device transfers of
any kind.

---

## Complete command inventory

### `0x9a`-prefix commands (init/reset family)

| Command | Encoding | Purpose |
|---------|----------|---------|
| `9a 10` | `0x109a` | Primary init/reset. Device echoes it back on EP_STATUS. |
| `9a 14` | `0x149a` | Init variant (sent at device open). |
| `9a 18` | `0x189a` | Init variant. |

### `0x40`-prefix 2-byte commands

| Command | Purpose / notes |
|---------|-----------------|
| `40 13` | Unknown — used in a small wrapper function, reads 2-byte response. |
| `40 1c` | Unknown. |
| `40 1e` | Unknown. |
| `40 21` | Unknown — appears in multiple functions. |
| `40 31` | Arm sensor (seen in pcap, matches Linux driver). |
| `40 3e` | Poll sensor status. Response scanned for `0xaa` (ready) or `0xff` (stop). |
| `40 3f` | Wait for finger. Response: `0x55` = finger present, `0xca` = retry. |
| `40 47` | Read calibration register (used during init calibration). |
| `40 57` | Unknown. |
| `40 67` | Read calibration register (used during init calibration). |
| `40 68` | Read calibration status (polled in a timed loop). |
| `40 7d` | Read calibration register (response used to set gain). |

### `0x00`-prefix 2-byte commands

| Command | Purpose |
|---------|---------|
| `00 09` | Capture frame — returns 15,600 bytes (15,552 + 48) on EP_IN (0x82). |
| `00 0b` | Init sequence — sent as part of startup. |

### `0x40`-prefix 3-byte commands (calibration only)

| Command | Notes |
|---------|-------|
| `40 87 XX` | Write config. `XX` = (sensor register value \| 0x80). |
| `40 8a 97` | Write config (static value). |
| `40 8b 71` or `40 8b 72` | Write config — two variants chosen by sensor state. |
| `40 8c 49` or `40 8c 69` | Write config — two variants chosen by sensor state. |
| `40 a8 78` | Write config (static value). |
| `40 bd XX` | Set gain. `XX` = (sensor register value & 0x3f). |

All 3-byte commands are fired with no response read. They are all issued
during a one-time calibration routine at device open, not during capture.

---

## Calibration sequence (at device open)

A function starting around `0x180005430` in `WbfUsbDriver.dll` implements the
sensor calibration. The logic is:

```
1. send [40 7d]  → read 1 byte (current gain register)
2. send [40 bd, gain & 0x3f]  → set gain (masked to lower 6 bits)
3. send [40 a8 78]  → static config write
4. poll [40 68] until bit 0x40 set (or 10 ms elapsed)
5. send [40 67]  → read status register
6. if (status & 0x06) == 0x06:
     send [40 47]  → read another register
     send [40 87, reg | 0x80]  → write it back with bit 7 set
     send [40 8a 97]  → static config
   else:
     if status & 0x01 == 0:
       send [40 8b 71], [40 8c 49]   → config variant A
     else:
       send [40 8b 72], [40 8c 69]   → config variant B
```

These are hardware-level gain and bias adjustments for the optical sensor
(common in under-glass optics to compensate for glass thickness/coating
variations). This has no bearing on fingerprint matching.

---

## EngineAdapter.dll

This DLL implements the WBF engine adapter interface. Its source path is
`Adapters-PB\engine_adapter`. Relevant strings found:

```
"Elan finger algorithm version = %d"
"sensor_type = %d"
"quick_verify = %d"
"MaxTemplateSize = %u"
"quick_verify_feature_num = %d"
"MaxEnrollNum_high_resolution = %d"
"MaxTemplateNum_high_resolution = %d"
"TemplateSetTH = %d"
"TemplateNear = %d"
```

The C++ type decorations include `VERIFY_INFO` and `VerifyInfo` structures,
and function names `_SA_VerifyTemplate_Set_inBuffer`, `_SA_VerifyTemplate_Get_data`,
`_SA_ConvertTemplateInfo`. All of these are host-side template processing —
the algorithm runs in the `EngineAdapter.dll` on the host, confirming that
matching is not on-chip.

---

## Conclusions

1. **No match-on-chip support.** The `[40 ff 01/03/13]` elanmoc command
   signatures are completely absent from all three binaries. This driver package
   has no MoC capability at all.

2. **Protocol is identical to the Linux driver.** The same five core commands
   (`9a 10`, `00 0b`, `40 31`, `40 3f`, `00 09`) are used. The Windows driver
   adds calibration commands at open time, but the capture protocol is unchanged.

3. **The 3-byte commands are calibration, not template transfer.** All 3-byte
   commands are sensor hardware configuration (gain/bias). None carry fingerprint
   data toward the sensor.

4. **The `[40 ff 01]` hypothesis is disproved.** The command is not present in
   this driver, and there is no mechanism by which it could be constructed from
   the existing code. The briefing's MoC hypothesis was reasonable given the
   elanmoc prefix similarity, but the binary evidence does not support it.

5. **Recommendation: pursue image-capture mode on Linux only.** The existing
   libfprint driver approach is correct. There is no basis for adding MoC support
   for this PID based on this driver package.

---

## Future work: calibration sequence

The Windows driver runs a one-time gain/bias calibration at device open that
the Linux driver does not currently implement.  This is hardware-level optical
compensation (common in under-glass sensors) that may improve image quality on
some hardware units.

The full calibration logic is documented in the command inventory above.  It is
not implemented in the current Linux driver because:
- The driver functions correctly without it in testing
- The benefit may be marginal if default gain is acceptable on most units
- It adds meaningful complexity (dynamic command values, polling loop with timeout)

Recommended as a follow-on improvement once the core driver is upstream.  The
implementation would add approximately 8-10 new states to the init SSM.
