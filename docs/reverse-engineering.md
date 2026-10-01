# acer-ec reverse-engineering report

## Scope

Reverse-engineering the ACPI DSDT and EC firmware on the Acer Aspire A715-79G
(i7-13620H, RTX 3050) to enable safe fan control on Linux without kernel
patches.

## Approach

Read-only analysis first (DSDT disassembly, EC protocol mapping, WMI probing),
then controlled write testing through established ACPI methods (SCMD).

---

## 1. DSDT disassembly

The DSDT was captured and disassembled to a 3.6M ASL file.

### Key ACPI objects

| Object | Path | Description |
|---|---|---|
| EC | `\_SB_.PCI0.LPC0.EC0_` | ITE 8222 embedded controller, IO ports `0x62/0x66` |
| WMI | `\_SB_.WMI` | ACPI WMI device, 3 GUIDs |
| RAM | `\_SB_.PCI0.LPC0.EC0_.RAM` | SystemMemory region at `0xFE0B0100`, 256 bytes |

### EC communication (ECMD method)

The EC uses a command-based protocol through RAM offsets `0xF8`-`0xFD`:

| Register | Offset | Purpose |
|---|---|---|
| FCMD | `0xF8` | Command byte |
| FDAT | `0xF9` | Data/index byte |
| FBUF | `0xFA` | Data buffer byte (write value) |
| FBF1 | `0xFB` | Buffer 1 |
| FBF2 | `0xFC` | Buffer 2 |
| FBF3 | `0xFD` | Buffer 3 |

**ECMD protocol** (reverse-engineered from 179 calls):

```
FCMD=0xC0, FDAT=index    → Read indexed register
FCMD=0xC1, FDAT=index, FBUF=value → Write indexed register

Index 0x00 = 0x14 (version/status)
Index 0x01 = 0x00 (fan 1 duty in auto mode?)
Index 0x02 = DUT1 (fan 1 duty)
Index 0x03 = DUT1 (duplicate — possibly fan 2 on other models)
Index 0x04 = 1
Index 0x05 = 0
Index 0x06 = 0
Index 0x07 = 64
Index 0x08-0x1F → return own index (invalid/return-self)
```

### WMI device

3 GUIDs discovered:

| GUID | Index | Purpose |
|---|---|---|
| `ABBC0F6B` | 0x6B | Unknown (present) |
| `ABBC0F6C` | 0x6C | Status/query (present) |
| `ABBC0F6D` | 0x6D | Control/event (present) |

The `WMBB` method dispatches sub-commands via Arg1:

| Arg1 | Method | Purpose |
|---|---|---|
| 0x02 | CPKG | OEM package |
| 0x03 | OCWR | OEM control write |
| 0x04 | SCMD | System command (many IDs) |
| 0x05 | GCMD | Get command (many IDs) |

### SCMD methods mapped

From DSDT analysis:

**SCMD 0x68** — Write duty cycle (4 bytes → 4 ECMD calls):
- Byte 0 → FDAT=0x01 (fan 1, overridden by EC)
- Byte 1 → FDAT=0x02 (fan 2 = GPU, works)
- Bytes 2-3 → FDAT=0x03, 0x04 (not connected)

**SCMD 0x69** — Set fan profile:
- Arg = `BIT(profile-1)`: 1→quiet, 2→balanced, 4→performance, 8→gaming

### SystemMemory layout (0xFE0B0100)

| Offset | Size | Name | Description |
|---|---|---|---|
| 0x07 | 1 | TMP | ACPI temperature (°C) |
| 0xCE | 1 | DUT1 | Fan 1 duty (EC-managed) |
| 0xCF | 1 | DUT2 | Fan 2 duty (GPU, writable) |
| 0xD0 | 2 | RPM1 | Fan 1 tach period |
| 0xD2 | 2 | RPM2 | Fan 2 tach period |
| 0xD4 | 2 | RPM4 | Always 0 (unused on this model) |
| 0xD7 | 1 | DTHL | Throttle level (bitmask) |
| 0xD8 | 1 | DTBP | Turbo boost power band |
| 0xD9 | 1 | AIRP | Airplane/temperature band |
| 0xDA | 1 | WINF | Windows interface flags |
| 0xDB | 1 | RINF | Radio info flags |
| 0xE0 | 2 | RPM3 | Always 0 (unused on this model) |

### Battery block (0x10-0x2F) — `_BIF` data

The EC also mirrors the ACPI `_BIF` battery information package into this
window. Values are little-endian 16-bit words. Identified 2026-10-01 by
cross-referencing a `dump` against live `power_supply` readings.

| Offset | Size | Decodes to | Evidence |
|---|---|---|---|
| 0x16 | 2 | Design capacity, mAh | 3410 = `charge_full_design` 3,410,000 µAh |
| 0x1A | 2 | Full capacity, mAh | 2850 = `charge_full` 2,850,000 µAh |
| 0x22 | 2 | Pack voltage, mV | 15399, tracks `voltage_now` |
| 0x2A | 2 | Charge current, mA | 1377 → 1261 across two dumps, tapering |
| 0x2E | 2 | Present charge, mAh | 402 → 579, tracks `charge_now` |
| 0x38 | 1 | State of charge, % | 15 → 21, exact match to `capacity` |
| 0x66 | 2 | Charge cutoff, mV | 15800, just above the observed CV plateau |

The design/full capacity pair matching to the exact milliamp-hour value
identifies the block: the coincidence of two independent numbers landing
correctly is not plausible by chance.

`0x38` is the strongest single confirmation. Dumps taken 2 min apart recorded
15 and 21 while `capacity` reported 15 % and 21 % — a direct byte-for-byte
match, not a correlation.

| Offset | Size | Contents |
|---|---|
| 0x5E | 4 | `"LION"` — ACPI lithium-ion chemistry tag |
| 0x4A | 20 | Pack descriptor string (`p71D-EBTSCUD-EBT@`) |

`0x66` is a **voltage** cutoff, not a state-of-charge percentage. Two
independent dumps show the EC tracking *current* state of charge (`0x38`) and
holding a voltage ceiling (`0x66`), but no byte stores a *target* percentage.
Acer's 80 % conservation mode is therefore either a firmware compile-time
constant or state held outside this region. See open question 6.

### No vendor driver exists

Checked before pursuing further RE:

- Acer ships no Linux tooling. `AcerSense` / `Acer Care Center` are Windows
  only. BIOS is current (1.07.01TACI, no fwupd update available).
- In-tree `acer-wmi` exposes 16 WMI methods, all fan / thermal / profile /
  hotkey. Zero battery, charger, calibration, or conservation strings in the
  module.
- No module on this system implements `charge_control_end_threshold`, the
  kernel charge-limit API. It exists for ThinkPad-class hardware; Acer has
  never implemented it.

The EC RE is not substituting for an available driver. The feature has no
Linux implementation at any layer.

---

## 2. Probed approaches (dead ends)

### NB05 IO-port path

Tried SIO bridge at `0x4E/0x4F` and `0x2E/0x2F` to access EC RAM. All
`nb05_read_ec_ram()` calls returned `0xFF`. The ITE 8222 EC on this board
communicates exclusively through ACPI IO ports `0x62/0x66`, not the LPC SIO
bridge.

### Uniwill WMI probe

The `uniwill` kernel module requires 6 WMI GUIDs (`ABBC0F6D` through `0F72`).
This platform has only 3 (`ABBC0F6B`, `0F6C`, `0F6D`). The module fails to
probe with `uniwill_interfaces.wmi = NULL`.

### Direct WMI calls on ABBC0F6D

Direct `wmi_query_block()` on GUID `ABBC0F6D` returns `0xFFFFFFFF` or fixed
values. No WMI method performs EC RAM I/O — all EC access routes through ACPI
method calls (`SCMD`, `ECMD`).

### Direct SystemMemory writes

Writing to the `0xFE0B0100` MMIO region via `ioremap` + `iowrite8` does change
the values momentarily, but the EC firmware overwrites them within ~500ms. Only
ECMD/SCMD ACPI calls produce persistent changes.

---

## 3. Verified findings

### Fan identification

CPU stress test (fan 1) vs idle (fan 2):

| Condition | DUT1 (Fan 1) | DUT2 (Fan 2) | TMP |
|---|---|---|---|
| Idle | 112 | 71 | 60°C |
| CPU stress | 255 (max) | 71 (unchanged) | 98°C |

Conclusion:
- **Fan 1 = CPU fan** (driven by CPU load, EC-protected)
- **Fan 2 = GPU fan** (writable, not affected by CPU load)

### RPM encoding

Raw values from SystemMemory are **tachometer periods**, not RPM:

```
RPM ≈ 120,000,000 / raw
```

Example: raw 40000 → 3000 RPM. The module applies this conversion.

The constant was originally documented as 60,000,000, which reports every fan
at exactly half its true speed. Verified against live tach data: raw 41731
yields 2875 RPM with the correct constant.

### Profiles verify
- Profile 1 (quiet): lowest fan speeds
- Profile 2 (balanced): moderate fan curve
- Profile 3 (performance): higher RPM before throttling
- Profile 4 (gaming): highest fan speeds

### WINF register suspected function

From DSDT code analysis, offset `0xDA` (WINF = Windows interface flags)
likely contains an "OS takeover" bit. When set, it may signal the EC that the
OS is managing fans directly, potentially disabling the EC override on DUT1.
**Not tested** — flipping this bit without understanding all flags carries risk
of unpredictable EC behavior.

---

## 4. Open questions

1. **WINF bit 0** — Does setting this disable EC override on DUT1? (Bit 7 of
   the 0x69 SCMD command already maps to "OS fan control" semantics in DSDT
   but is never issued.)
2. **ECMD registers 0x00-0x07** — Thermal trip points? Configuration
   registers?
3. **Additional temperature sensors** — Does EC RAM contain other sensor
   values beyond TMP (offset 0x07)?
4. **Tach-to-RPM constant** — Resolved. 120,000,000, verified against live
   tach values.
5. **MXM_WMMX_GUID** — GUID `F6CB5C3C-9CAE-4EBD-B577-931EA32A2CC0` (NVIDIA
   Optimus display routing) present but not fan-related.
6. **Charge-limit control** — Is Acer's conservation mode (stop at 80 %)
   settable at all? The `_BIF` block at `0x10`–`0x2F` exposes telemetry and a
   voltage cutoff at `0x66`, but no percentage register. Needs a BIOS-side
   test: enable battery health/calibration in firmware, re-dump, and check
   whether any byte in the window changes.

---

## 5. Extra WMI GUIDs found

Beyond the 3 Acer GUIDs, these standard Microsoft WMI GUIDs are also exposed
(and not relevant to fan control):

| GUID | Purpose |
|---|---|
| `F6CB5C3C-9CAE-4EBD-B577-931EA32A2CC0` | MXM_WMMX_GUID — NVIDIA Optimus mux |
| `95F24279-5BFB-4302-8731-15307C0B06A5` | WMI method for OEM interface |

---

## 6. Methods summary for future EC work

### ACPI SCMD method signatures

```
SCMD(0x68, buf[4])  →  Write individual EC fields via ECMD
    buf[0] → duty channel 1 (EC overridden)
    buf[1] → duty channel 2 (GPU fan, works)

SCMD(0x69, BIT(n))  →  Set fan profile
    n=0 → quiet       (value 1)
    n=1 → balanced    (value 2)
    n=2 → performance (value 4)
    n=3 → gaming      (value 8)
```

### ECMD method (for reference)

```asl
Method (ECMD, 3, Serialized)
{
    Store (Arg0, FCMD)
    Store (Arg1, FDAT)
    If (LEqual (Arg2, One))  // write operation
    {
        Store (Arg3, FBUF)
    }
    Store (One, FBFC)        // Trigger
    While (LEqual (FBFC, One)) { }  // Wait for completion
    Return (FBUF)
}
```

Values: FCMD=0xC0 (read), FCMD=0xC1 (write). The index/data byte selects
the internal EC register, not the SystemMemory offset. There is a mapping
layer in the EC firmware.
