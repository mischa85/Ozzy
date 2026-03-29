# Vendor Request 'I' (0x49) — Hardware Control Registers

Sources: USB traffic capture, Windows driver reverse engineering, macOS driver symbol names.
Cross-driver confirmation: Windows and macOS drivers match exactly (same masks, same read-modify-write patterns).

## General Structure

```
bmRequestType: 0x80 (read) / 0x00 (write)
bRequest:      0x49 ('I')
wIndex:        selects sub-register
```

Three known indices:
- `wIndex = 0` → AJ input selector / digital status
- `wIndex = 1` → US3XX / mixer / CPLD config
- `wIndex = 2` → digital output selector (write path, not relevant for decoding return byte)

Reads return 1 byte in the data phase. Writes encode the value in wValue.

---

## Index 0 — AJ Input Selector / Digital Status

Used in: `updateAjInputSelector`, `setAJDMAInputChannels`, digital lock detection paths.

### Confirmed bits (from Ghidra RE of official macOS driver)

```
bit 0x01 → USB1 mode (set by setUSB1Mode, stored in AjExtData[2])
bit 0x02 → clock source select (set by setClockSource, stored in AjExtData[1])
           AjExtData[1]==0 → set bit (|0x02), non-zero → clear bit (&0xFD)
bit 0x04 → digital lock (confirmed via (byte & 0x04) != 0; triggers re-read after 20ms sleep)
```

### Behavior

- `0x04` indicates digital signal lock
- `0x02` = clock source selection (internal vs external), not generic "input select"
- `0x01` = USB1 mode flag, only set on certain devices (those with `this[0x7a8]` set,
  or product 0x644 sub-IDs 0x8048/0x8041)
- Driver masks with `0xFD` and recomputes bit 1 from current clock source setting
- Read-modify-write: reads status, modifies bits 0-1 from software state, writes back

### Other bits

```
0x08, 0x10, 0x20, 0x40, 0x80
```

- Frequently masked/merged (`& 0xE7`, `& 0x4C`)
- Device-dependent
- Used to encode routing / timing class / hardware mode
- For product 0x644: bits 3-4 (mask `0x18`) come from `this[0xd2]`, rest preserved with `& 0xE7`

### When updateAjInputSelector is called (from Ghidra)

1. **`configurationDone`** — device init, called twice (before and after `chooseISOOutEncoder`)
2. **`setClockSource`** — clock source change: suspend streaming → update → resume
3. **`setUSB1Mode`** — USB mode switch: suspend streaming → update → resume

### _globDummyAjExtData / PtAjExtData structure (inferred)

```
byte +0x00 [0]  → digital lock frequency (read via GET_CUR after write-back)
byte +0x04 [1]  → clock source (PTE_ClockSource enum, controls bit 1 of status byte)
                   For ESU: 2→STATE_A(0x32), 1→STATE_B(0xB0), other→STATE_C(0xB2)
                   Dispatch: read=0x0F, write=0x10 (setClockSource)
byte +0x08 [2]  → USB1 mode (PTE_USB1Mode enum, controls bit 0 of status byte)
                   Dispatch: read=0x02/0x19, write=0x1A (setUSB1Mode)
byte +0x0C [3]  → digital out mode
                   Dispatch: read=0x1B, write=0x1C (setDigitalOutMode)
byte +0x10 [4]  → digital output selector value (written to index 2 by writeDigitalOutSelector)
                   Dispatch: write=0x03 (setDigitalOutSelector)
byte +0x14      → input postprocessor setting [sub=0]
byte +0x18      → input postprocessor setting [sub=1]
byte +0x1C      → input postprocessor setting [sub=2]
byte +0x20      → output postprocessor setting [sub=0]
byte +0x24      → output postprocessor setting [sub=1]
byte +0x28 [10] → digital lock flag (written from bit 2 of status byte)
```

### ESU composite states (from updateAjInputSelector_Esu186)

The ESU path preserves bits with mask `0x4C`, then ORs in a composite state
based on the clock source setting. These are NOT individual flags:

| AjExtData[1] | State | Value | 'A' request param |
|---------------|-------|-------|-------------------|
| 2             | STATE_A | 0x32 | 0 (wValue=0x348) |
| 1             | STATE_B | 0xB0 | 1 (wValue=0x349) |
| other         | STATE_C | 0xB2 | 0 (wValue=0x348) |

After setting the state, the ESU path sleeps **200ms** (vs ~40ms for other devices).

---

## Index 1 — US3XX / Mixer / CPLD Config

Used in: `readUS3XXChannelConfig`, mixer mode switching (`Req_SetMixerMode`), CPLD handling.

### Confirmed bits

```
bit 0x02 → open control panel switch (active-low)
bit 0x08 → mixer layout / width (e.g. 4ch vs 6ch)
bit 0x10 → mixer bypass / enable flag
```

### Mixer mode handling (from Req_SetMixerMode, dispatch case 0x26)

The dispatch reads index 1 then modifies bit 4 (0x10) based on mixer mode:

| Mode | Bit 4 | Effect | Channel config (0x644 devices) |
|------|-------|--------|-------------------------------|
| 0    | SET (OR 0x10) | Mixer bypass | 2in / 2out |
| 1    | CLEAR (AND 0xEF) | Mixer active | 0x8048: 4in/4out, 0x8046: 6in/2out |
| 2    | CLEAR (AND 0xEF) | Mixer active | Same as mode 1 |

The modified byte is stored to `this[0xd2]` (the channel config byte used in
`updateAjInputSelector` for product 0x644 index 0 writes with mask 0x18).

This confirms `this[0xd2]` is the **mixer config register** — it holds the
last-written index 1 byte, and its bits 3-4 are re-used when writing index 0.

### Behavior

- `0x10`:
  - set → mixer bypass / simple mode (2ch in/out)
  - clear → mixer active (multi-channel routing, 4ch or 6ch)
- `0x08`:
  - influences channel layout (4ch vs 6ch)
- `0x02`:
  - physical switch (active-low)

### setEsuCpldByte — confirmed index 1 write (from Ghidra)

For ESU (0x2573) and 0x0A92/0x0111 only:
- Applies `| 0xE7` to the input byte (forces high mask confirmed!)
- Writes to vendor request 'I', wIndex=1 with the masked value
- Stops bulk audio before writing, resumes after
- Stored in `this[0x55D8]`

### readUS3XXChannelConfig — confirmed index 1 read (from Ghidra)

For VID 0x644 (TEAC) devices. Reads 'I' wIndex=1, stores in `this[0xD2]`.
Product names from log strings:
- 0x8046: "PROD_TEAC_US1200"
- 0x8048: "PROD_TEAC_UH7000"
- 0x8041: "US-366" (most complex: dynamic 2/4/6 channel based on mixer bits)
- 0x8040: unnamed (simple 2ch check)

For 0x8041, the mixer byte controls channel topology:
- bit 4 set (0x10): mixer bypass → 2in/2out
- bit 4 clear + bit 3 clear: 4ch mode → 4out/6in
- bit 4 clear + bit 3 set: 6ch mode → 6out/4in

After reading index 1, always writes routing table via vendor request 'M' (0x4D).

### Write constraints

```
write mask: 0x18 (bits 3 and 4)
forced high mask: 0xE7
```

Only bits `0x08` and `0x10` are writable. Other bits are preserved or forced.

---

## Index 2 — Digital Output Selector (from writeDigitalOutSelector)

```
bmRequestType: 0x40 (write)
bRequest:      0x49 ('I')
wValue:        AjExtData[4] & 0xFFFF
wIndex:        2
wLength:       0
```

- Write-only, no read-modify-write
- Only written if device has digital output capability (`this[0x7ac]` set)
- Value comes directly from `AjExtData[4]`
- Called during `configurationDone` after `updateAjInputSelector`

---

## Cross-Driver Confirmation

Windows and macOS drivers match exactly:
- Same read-modify-write pattern
- Same bit masks (`0xDF`, `0xE7`, etc.)
- Same logic for digital lock detection, mixer mode switching, input selector updates

### macOS Function Name → Meaning Map

| Function                 | Meaning                |
|--------------------------|------------------------|
| updateAjInputSelector    | index 0 semantics      |
| setAJDMAInputChannels    | index 0 mode switching |
| readUS3XXChannelConfig   | index 1 semantics      |
| setEsuCpldByte           | index 1 write path     |

---

## Enum Definitions

```c
typedef enum VendorRequest : uint8_t {
    VENDOR_REQ_I = 0x49,
} VendorRequest;

typedef enum AjInputSelectorFlags : uint8_t {
    AJ_INPUTSEL_MODE0_BIT        = 0x01,
    AJ_INPUTSEL_INPUT_SELECT_BIT = 0x02,
    AJ_INPUTSEL_DIGITAL_LOCK_BIT = 0x04,
    AJ_INPUTSEL_MODE3_BIT        = 0x08,
    AJ_INPUTSEL_MODE4_BIT        = 0x10,
    AJ_INPUTSEL_MODE5_BIT        = 0x20,
    AJ_INPUTSEL_MODE6_BIT        = 0x40,
    AJ_INPUTSEL_MODE7_BIT        = 0x80,
} AjInputSelectorFlags;

typedef enum Us3xxChannelConfigFlags : uint8_t {
    US3XXCFG_OPEN_CPL_SWITCH_N = 0x02,
    US3XXCFG_MIXER_WIDTH_BIT   = 0x08,
    US3XXCFG_MIXER_BYPASS_BIT  = 0x10,
} Us3xxChannelConfigFlags;
```

### Supporting Constants

```c
#define AJ_INPUTSEL_DIGITAL_LOCK_MASK  0x05
#define AJ_INPUTSEL_DIGITAL_LOCK_VALUE 0x04

#define US3XXCFG_WRITABLE_MASK         0x18
#define US3XXCFG_FORCED_HIGH_MASK      0xE7

#define AJ_INPUTSEL_ESU_PRESERVE_MASK  0x4C
#define AJ_INPUTSEL_ESU_STATE_A        0x32
#define AJ_INPUTSEL_ESU_STATE_B        0xB0
#define AJ_INPUTSEL_ESU_STATE_C        0xB2
```

---

## wValue High Byte Mystery — SOLVED (sign extension artifact)

From Ghidra decompilation of the official macOS Ploytec driver (`PGDevice::updateAjInputSelector`),
the write-back uses:

```c
(short)(char)bVar12
```

This sign-extends the byte through `char` (signed) to 16-bit `short`:
- If bit 7 is set (e.g. `0xB2`): `(char)0xB2` = -78 → `(short)(-78)` = `0xFFB2`
- If bit 7 is clear (e.g. `0x32`): `(char)0x32` = +50 → `(short)(50)` = `0x0032`

**The high byte is not a second register.** It is purely a sign-extension artifact in the
original Ploytec driver. The device likely ignores it.

### Evidence

| Device | Index 0 read | After OR 0x20 | Bit 7 | wValue (sign-extended) |
|--------|-------------|---------------|-------|------------------------|
| DB4    | 0x92        | 0xB2          | set   | 0xFFB2                 |
| DB2    | 0x12        | 0x32          | clear | 0x0032                 |

USB capture from DB2 confirms the official driver sends `wValue = 0x0032`.

### Bug in our driver

`ploytec.c:ploytec_confirm_status()` hardcodes `wvalue = 0xFF00 | modified`, which
always forces the high byte to 0xFF. This is wrong for devices where bit 7 is clear
(e.g. DB2 → sends 0xFF32 instead of 0x0032).

Fix: `wvalue = (uint16_t)(int16_t)(int8_t)modified;`

---

## updateAjInputSelector — Ghidra decompilation analysis

From official macOS driver class `PGDevice` (called on `PGKernelDeviceXONEDB4`).

### deviceRequestStd parameter order (inferred)

```
deviceRequestStd(device, direction, bmRequestType_base, ?, bRequest, wValue, wIndex, data_ptr, length_ptr, timeout)
```

- direction 0x80 = IN (read), 0x00 = OUT (write)
- bmRequestType_base 0x40 = vendor, 0x20 = class
- bRequest 0x49 = 'I', 0x81 = GET_CUR

### Observed behavior

1. Reads index 0: `deviceRequestStd(dev, 0x80, 0x40, 0, 0x49, 0, 0, &byte, &len, 2)`
2. If digital lock bit set (`byte & 0x04`), sleeps 20ms and re-reads
3. Device-specific branching on product IDs at offsets `this+0xa86` and `this+0xa88`:
   - `0x644` = one product family (checks sub-IDs: 0x8041, 0x8040, 0x8048, 0x8046)
   - `0xa4a` = another product family (checks sub-ID: 0xFFDD)
4. Read-modify-write with masks `0x18` and `0xE7` (same as index 1 write constraints)
5. Modifies bit 1 (0x02) and bit 0 (0x01) based on external data (`_globDummyAjExtData`)
6. Write-back uses `(short)(char)bVar12` — sign-extending the byte
7. After write, re-reads index 0 and also reads sample rate via class request (0x81, GET_CUR)
8. Handles digital lock frequency detection with retries (up to ~21 iterations)
9. References `can192()` — checks if device supports 192kHz (0x2ee00 Hz)

### ESU special path

Devices with product ID 0x2573 and sub-ID < 0x2B with specific bit patterns
branch to `updateAjInputSelector_Esu186()` — completely separate logic.

---

## Key Takeaways

- **Not a simple flag register** — it is a stateful control byte
- Index 0 = input + digital lock + mode encoding
- Index 1 = mixer + CPLD + physical switch state
- Many bits must be treated as device-specific composite state, not standalone flags
- Read-modify-write is mandatory; never blindly overwrite
- wValue high byte on writes is a sign-extension artifact, not meaningful data
