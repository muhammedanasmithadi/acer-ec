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

**ECMD protocol** (reverse-engineered from 179 calls, re-checked against
`dsdt.dsl:100640`):

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

Confirmed against the firmware tables captured 2026-10-02. `\_SB.WMI` sits
at `dsdt.dsl:95731` and its `_WDG` buffer at `dsdt.dsl:95736` holds exactly
these three entries, each followed by a 4-byte instance/flags field. None
appear in `acer_wmi_id_table[]`, which is why the in-tree driver never
binds. The GUID tails (`8EA1-11D1-00A0-C90629100000`) are a recurring
Microsoft sample-code pattern rather than an Acer-assigned namespace, so
treat "these GUIDs mean anything specific" as unproven — the dump shows
only that the board exposes them.

The `WMBB` method dispatches sub-commands via Arg1:

| Arg1 | Method | Purpose |
|---|---|---|
| 0x02 | CPKG | OEM package |
| 0x03 | OCWR | OEM control write |
| 0x04 | SCMD | System command (many IDs) |
| 0x05 | GCMD | Get command (many IDs) |

### SCMD methods mapped

From the firmware tables (captured 2026-10-02, `dsdt.dsl`):

**SCMD 0x68** (`dsdt.dsl:97162`) — Write duty cycle, 4 bytes → 4 ECMD calls:
- Byte 0 → EC index 0x01
- Byte 1 → EC index 0x02
- Byte 2 → EC index 0x03
- Byte 3 → EC index 0x04

**SCMD 0x69** (`dsdt.dsl:97197`) — Run selected channels at full duty.
`ARGS` is a 4-bit channel mask; each set bit writes the literal `0xFF` to
one EC index:

| `ARGS` bit | EC index written | Value |
|---|---|---|
| `0x01` | 0x01 | `0xFF` |
| `0x02` | 0x02 | `0xFF` |
| `0x04` | 0x03 | `0xFF` |
| `0x08` | 0x04 | `0xFF` |

**This is not a profile selector.** There is no quiet/balanced/performance/
gaming encoding in the firmware. An earlier draft of these docs read the
`0xFF` write as a profile number and mapped `BIT(profile-1)` to
1=quiet, 2=balanced, 3=performance, 4=gaming. Under the actual code that
mapping is inverted in effect: "quiet" pins fan 1 to maximum. The same
`0x68`/`0x69` bodies are duplicated verbatim in `\_SB.DCHU.SCMD`
(`dsdt.dsl:106617` and `dsdt.dsl:106652`).

Sweeping every literal `ECMD` argument buffer in the DSDT turns up only
these EC register writes: duty `0x00` and `0xFF` on indices 0x01-0x04, plus
`0xC4` (event enable, index 0x05), `0xC2`, and `0xD8`. No profile register
exists to set.

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
| 0x1A | 2 | Full capacity, mAh | 2844 = `charge_full` 2,844,000 µAh (2026-10-02, charging); gauge relearned from 2704 across the full-drain event |
| 0x22 | 2 | Design voltage, mV (constant) | 15400 in every dump while the real pack moved 14.26 → 16.29 V; equals `voltage_min_design` 15,400,000 µV, not live telemetry |
| 0x2A | 2 | Charge current, mA | 2925 = `current_now` 2,925,000 µA to the digit (2026-10-02, charging); 0 while stalled, matching sysfs exactly |
| 0x2E | 2 | Present charge, mAh | 179 = `charge_now` 179,000 µAh to the digit (2026-10-02, charging); 0 while stalled |
| 0x38 | 1 | State of charge, % | 7–8 vs `capacity` 6–7 (2026-10-02, charging — within 1 pp, rounding); 0 while stalled |
| 0x66 | 2 | Unidentified counter (NOT a cutoff) | 17000 while charging; 0,0,0,0,0,3208 then 3216→…→8 while stalled — counts down and reloads, which no voltage ceiling does |

The design/full capacity pair matching to the exact milliamp-hour value
identifies the block: the coincidence of two independent numbers landing
correctly is not plausible by chance. The 2026-10-02 recovery run verified
`0x2A`/`0x2E`/`0x16`/`0x1A` against sysfs to the exact digit in both the
stalled and charging states, so EC and kernel agree and neither path
misreports.

`0x38` is the strongest single confirmation. Dumps taken 2 min apart recorded
15 and 21 while `capacity` reported 15 % and 21 % — a direct byte-for-byte
match, not a correlation.

| Offset | Size | Contents |
|---|---|
| 0x5E | 4 | `"LION"` — ACPI lithium-ion chemistry tag |
| 0x4A | 20 | Pack descriptor string (`p71D-EBTSCUD-EBT@`) |

`0x66` was previously documented as a 15800 mV charge cutoff. Withdrawn
2026-10-02: it read 17000 while charging and counted down (3208 → 8,
including stable 0 reads) while stalled. A voltage ceiling does not count
down and reload, so the cutoff interpretation has no support left; the
register's function is unidentified. No byte in the window stores a
*target* percentage, so Acer's 80 % conservation mode — if it exists on
this platform — is either a firmware compile-time constant or state held
outside this region. See open question 6.

The block is exposed read-only by `acer_ec_debug` as `reg16_16` /
`reg16_1A` / `reg16_22` / `reg16_2A` / `reg16_2E`, `reg8_38`, `reg16_66`,
plus a decoded `batt` node — see `docs/charging.md` for the probe ladder.

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

### Profiles do not verify

The earlier claim that profile 1/2/3/4 produce ordered fan curves is
withdrawn. Two independent lines of evidence kill it:

1. The firmware has no profile encoding. SCMD 0x69 is a 4-bit channel mask
   that writes `0xFF` (see §1), so the four "profiles" differ only in
   *which* channel gets pinned to full.
2. The controlled A/B in the README (profiles 2/3/4 × 2/4/8-core loads)
   measured fan 1 frozen at ~2700 RPM on every combination, with package
   temperature 94-96 °C and no separation between profiles. That is the
   expected result if the writes target the wrong registers, not evidence
   of overlapping fan curves.

The single ~4900 RPM observation is not a controlled run and cannot
support a ramp claim. Any fan behavior on this platform is still the EC
acting on its own sensors, not a Linux-selected profile.

### WINF register suspected function

From DSDT code analysis, offset `0xDA` (WINF = Windows interface flags)
likely contains an "OS takeover" bit. When set, it may signal the EC that the
OS is managing fans directly, potentially disabling the EC override on DUT1.
**Not tested** — flipping this bit without understanding all flags carries risk
of unpredictable EC behavior.

---

## 4. Open questions

1. **WINF bit 0** — Does setting this disable EC override on DUT1? (No
   SCMD in the DSDT issues it; the only writer of `0xDA` is the Fn-key
   event path in SCMD 0x46.)
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
SCMD(0x68, ARGS)  →  Write four duty bytes via ECMD
    ARGS[0x00] → EC index 0x01
    ARGS[0x08] → EC index 0x02
    ARGS[0x10] → EC index 0x03
    ARGS[0x18] → EC index 0x04

SCMD(0x69, ARGS)  →  Run selected channels at full duty
    ARGS & 0x01 → write 0xFF to EC index 0x01
    ARGS & 0x02 → write 0xFF to EC index 0x02
    ARGS & 0x04 → write 0xFF to EC index 0x03
    ARGS & 0x08 → write 0xFF to EC index 0x04

Both bodies are duplicated in \_SB.DCHU.SCMD.
```

`0x69` is a channel mask, not a profile selector. Any code that maps
`BIT(n)` to a named fan profile is writing the wrong register — see §3.

### ECMD method (for reference)

`dsdt.dsl:100640` declares one buffer argument, not four positional ones:

```asl
Method (ECMD, 1, NotSerialized)
{
    // Arg0 is an 8-byte buffer
    Local1 = DerefOf (Arg0 [0x00])   // byte count selector
    Local2 = DerefOf (Arg0 [0x01]) & 0x80
    FDAT   = DerefOf (Arg0 [0x03])
    FBUF   = DerefOf (Arg0 [0x04])
    FBF1   = DerefOf (Arg0 [0x05])
    FBF2   = DerefOf (Arg0 [0x06])
    FBF3   = DerefOf (Arg0 [0x07])
    FCMD   = DerefOf (Arg0 [0x02])
    Return (FCMD, FDAT, FBUF, FBF1, FBF2, FBF3)
}
```

Field offsets: `Arg0[2]` is FCMD, `Arg0[3]` is the register index, and
`Arg0[4]` onward are the data bytes, each written only when
`Arg0[0]` is large enough to include it. So a register write is the
literal buffer `{ 0x03, 0x00, 0xC1, <index>, <value>, 0x00, 0x00, 0x00 }`.

Values: FCMD=0xC0 (read), FCMD=0xC1 (write). The index/data byte selects
the internal EC register, not the SystemMemory offset. There is a mapping
layer in the EC firmware.
