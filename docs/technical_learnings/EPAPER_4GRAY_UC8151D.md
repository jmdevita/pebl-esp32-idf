# E-Paper 4-Level Grayscale on UC8151D (IL0373)

## Summary

The GDEW0213I5F 2.13" e-paper display uses the UC8151D controller (also labeled IL0373). This document covers how 4-level grayscale works, what can go wrong, and the pre-clear pattern required for reliable rendering.

## How 4-Gray Mode Works

### Dual Bit Planes (DTM1 + DTM2)

In standard B&W mode, the UC8151D treats its two data registers as "old" and "new" pixel states:
- **DTM1** (command `0x10`): Previous pixel data ("old")
- **DTM2** (command `0x13`): Current pixel data ("new")

The controller compares old vs new for each pixel and selects one of four LUT waveforms:
- **WW** (`0x21`): white to white (bit 1 -> 1)
- **BW** (`0x22`): black to white (bit 0 -> 1)
- **WB** (`0x23`): white to black (bit 1 -> 0)
- **BB** (`0x24`): black to black (bit 0 -> 0)

**In 4-gray mode, DTM1 and DTM2 are repurposed as two bit planes of the same image.** Each pixel gets one bit from each plane, encoding 4 levels:

| Gray Level   | DTM1 bit | DTM2 bit | LUT Used |
|-------------|----------|----------|----------|
| White       | 1        | 1        | WW       |
| Light Gray  | 1        | 0        | WB       |
| Dark Gray   | 0        | 1        | BW       |
| Black       | 0        | 0        | BB       |

The 4-gray LUTs are crafted so each "transition" waveform drives pixels to a specific absolute gray level rather than performing a state transition. For example, the WB LUT doesn't drive "white to black" — it drives to the light gray voltage level.

### LUT Waveform Differences

The 4-gray LUTs use significantly shorter waveforms than B&W full-update LUTs:

| LUT Set     | Approximate Total Cycles | Purpose                    |
|-------------|--------------------------|----------------------------|
| B&W Full    | ~168 cycles              | Maximum driving force      |
| 4-Gray      | ~92 cycles               | Precision gray levels      |
| B&W Partial | ~40 cycles               | Fast, minimal transition   |

The gray LUTs are shorter because they need to stop at intermediate voltage levels. Driving too hard would overshoot the target gray level and produce B&W instead of gray.

## The Pre-Clear Requirement

### The Problem

E-paper displays retain their image with no power. When the device boots, the physical pixel state is whatever was displayed in the previous session — completely unknown to the controller. After a hardware reset (`IO.reset()`), the controller's internal RAM defaults to all-white (0xFF), but the physical display doesn't change.

When gray LUTs are applied:
- The controller selects waveforms based on the DTM1/DTM2 bit-plane encoding
- Each waveform assumes it's driving from a **known starting state**
- But the physical pixels are in an **unknown state** from the previous session
- The weaker gray waveforms (~92 cycles) can't reliably overcome this mismatch
- Result: partial rendering — some pixels transition correctly, others don't

### The Symptom

After boot, the first gray screen would render incompletely. For example, "Waiting for Reactions" text might appear only in part of the screen, or old content from a previous session would bleed through. This looked like "partial refresh" artifacts rather than classic ghosting.

### The Solution: One-Time B&W Pre-Clear

Before the first gray update after boot, perform a full B&W refresh to white:

```cpp
// Phase 1: B&W clear — strong waveforms force all pixels to known white state
initFullUpdate();           // Load B&W LUTs (~168 cycles)
IO.cmd(0x10);               // DTM1 = white
for (i = 0; i < BUF_SIZE; i++) IO.data(0xFF);
IO.cmd(0x13);               // DTM2 = white
for (i = 0; i < BUF_SIZE; i++) IO.data(0xFF);
IO.cmd(0x12);               // Refresh
waitBusy();

// Phase 2: Gray content — LUTs transition from known-white to target levels
initGrayUpdate();           // Load 4-gray LUTs
IO.cmd(0x10);               // DTM1 = bit plane 1
IO.data(buffer1, BUF_SIZE);
IO.cmd(0x13);               // DTM2 = bit plane 2
IO.data(buffer2, BUF_SIZE);
IO.cmd(0x12);               // Refresh
waitBusy();
```

This only needs to happen once. After the first gray update, the display's physical state matches what the controller expects, so subsequent gray updates work correctly without a pre-clear.

### Implementation

In `gdew0213i5f.cpp`, a `_gray_needs_clear` flag (initialized `true`) triggers the pre-clear on the first gray `update()` call, then is set to `false`. This adds ~2 seconds to the first screen after boot but all subsequent updates are normal speed.

This matches the GxEPD2_4G reference library, which uses an `_initial_write` flag for the same purpose.

## Register Configuration

### Registers That Matter

| Register | Name                       | B&W Value | Gray Value | Notes                                    |
|----------|----------------------------|-----------|------------|------------------------------------------|
| `0x82`   | VCOM DC Setting            | `0x08`    | `0x08`     | VCOM voltage offset (~-0.6V)             |
| `0x50`   | VCOM and Data Interval     | `0x97`    | `0x97`     | Border=LUTBB, data polarity, interval=7  |
| `0x00`   | Panel Setting              | `0xBF`    | `0xBF`     | LUT from register, 128x296, KW mode      |
| `0x01`   | Power Setting              | Standard  | Standard   | VDH/VDL = +/-11V, VDHR = +3V            |

### The 0x50/0x82 Regression

During development, `initGrayUpdate()` was changed to skip registers `0x50` and `0x82` (VCOM voltage and data interval), with the rationale that GxEPD2_4G's `_Init_4G()` also skips them. However, GxEPD2_4G relies on these being set by `_InitDisplay()` earlier in its init sequence. In our code, `_wakeUp()` does a hardware reset which clears these to chip defaults — leaving them unset meant weaker pixel driving, which compounded the unknown-starting-state problem.

The proper fix was the pre-clear pattern (above), not register tweaks. Setting 0x50/0x82 in gray mode doesn't hurt but doesn't solve the fundamental issue of unknown physical pixel state on boot.

## References

- [GxEPD2_4G](https://github.com/ZinggJM/GxEPD2_4G) — Reference 4-gray implementation for Arduino
- [UC8151D Datasheet](https://cursedhardware.github.io/epd-driver-ic/UC8151d.pdf)
- [antirez UC8151 MicroPython Driver](https://github.com/antirez/uc8151_micropython) — Includes `full_update_period` for periodic deghosting
- [u8g2 Issue #1393](https://github.com/olikraus/u8g2/issues/1393) — Community LUT research for UC8151D
- [CalEPD](https://github.com/martinberlin/CalEPD) — ESP-IDF e-paper component library (base for our driver)
- Good Display demo code — Original source of the 4-gray LUT waveform values
