# CachedPhoneTracker — Meshtastic GPS Tracker Module

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

A Meshtastic **optional module** that turns a compatible GPS-equipped board into a standalone
GPS tracker with **offline point caching** and **phone sync**. Four hardware targets are
supported:

| Target | Chip / screen | UI |
|---|---|---|
| Heltec Wireless Tracker V2 | ESP32-S3, color TFT | Status card + sub-menu |
| Elecrow ThinkNode M1 | nRF52840, e-ink | Status card + sub-menu (e-ink refresh handled) |
| Heltec Mesh Node T096 | nRF52840, color TFT (TFT_eSPI) | Status card + sub-menu |
| Seeed T1000-E | nRF52840, **no screen** | Button gestures + buzzer feedback |

> ⚠️ This is **not** a complete firmware. It is a drop-in Meshtastic module — two source files
> that you copy into an existing Meshtastic source tree. The module is auto-discovered by
> Meshtastic's `optional-modules.py` build hook. The T1000-E also needs a small patch to
> Meshtastic's `ButtonThread.cpp` (supplied in `patches/`).

---

## What it does

- Logs the current GPS position to a local **LittleFS** file at the same rate Meshtastic refreshes
  the GPS (the app's **"Position update interval"** = `position.gps_update_interval`: 8/20/40/60/80/120 s)
  while tracking is enabled.
- Caches up to **500** points (`MAX_CACHED_POSITIONS`), rotating out the oldest half when full.
- **Phone sync** of every cached point:
  - **Screen builds** — via the **"Send cache"** menu item.
  - **T1000-E** — **automatically on phone connect** (after a 5 s settle delay so the app can
    finish reading config/nodes before the cache starts streaming).
- Sync is **non-blocking and paced** — one entry per loop cycle, ~1 s apart
  (`TRACKER_SYNC_INTERVAL_MS`), so the BLE queue isn't flooded, points aren't dropped, and the
  module thread never blocks long enough to trip the watchdog.
- Screen builds show a **status card** (`Tracking: ON/OFF`, `Cache: N/500`) and a single-button
  **sub-menu**. The screenless T1000-E uses **button gestures** and **buzzer tunes** instead.

## Before flashing — back up first
   - Export and safely store your Meshtastic configuration, including channel settings and keys.
   - Enter the device's bootloader mode and copy its existing CURRENT.UF2 to a safe location before installing this firmware. Keep it so you can restore the previous firmware if needed.

## On-device UI

### Screen builds (Heltec V2, ThinkNode M1, Heltec T096)

| Action | Behaviour |
|---|---|
| **Long-press** on the card | Opens the sub-menu |
| **Short-press** | Cycles `Toggle tracking` → `Send cache` → `Clear cache` → `Back` |
| **Long-press** | Activates the highlighted item |

> Single-button boards map **short-press → `INPUT_BROKER_USER_PRESS`** and
> **long-press → `INPUT_BROKER_SELECT`** (not `SELECT_LONG`). A very long hold still powers the
> board off — that is the stock Meshtastic `SHUTDOWN` mapping.

### T1000-E (screenless — button gestures)

| Clicks | Action | Buzzer feedback |
|---|---|---|
| 1 | Log a waypoint now | beep on successful save |
| 2 | Toggle tracking on/off | ascending tune (ON) / descending tune (OFF) |
| 3 | Clear the cached points | low descending "dump" |
| 4 | GPS toggle | beep |
| 5 | Node status | beep |

While tracking, each saved waypoint plays a **single beep** so you can hear it's alive and
logging.

## Defaults

- Tracking starts **OFF on every boot** (the enabled state is deliberately **not** persisted).
- Cache cap is **500** points (`MAX_CACHED_POSITIONS` in `CachedPhoneTracker.h`).
- Log interval = the app's **"Position update interval"** (`position.gps_update_interval`,
  8/20/40/60/80/120 s; default 120 s). No fixed timer.

## Files

| File | Purpose |
|---|---|
| `CachedPhoneTracker.h` | Module declaration, `TrackPoint` binary record, constants |
| `CachedPhoneTracker.cpp` | Implementation: logging, cache I/O, sync, UI, gestures, buzzer |
| `patches/ButtonThread.cpp` | Modified Meshtastic `src/input/ButtonThread.cpp` — routes 1–5 click gestures to `handleMultipress()` on `TRACKER_T1000_E` |

## Integrating into Meshtastic

1. Copy the module into your Meshtastic tree:

   ```
   src/modules/optional/CachedPhoneTracker/CachedPhoneTracker.h
   src/modules/optional/CachedPhoneTracker/CachedPhoneTracker.cpp
   ```

2. **T1000-E only** — replace `src/input/ButtonThread.cpp` with `patches/ButtonThread.cpp`
   (adds three `#ifdef TRACKER_T1000_E` hooks in the button dispatch; the `#else` branches are
   the unmodified stock code, so non-T1000-E targets are unaffected).

3. Rebuild your target. Meshtastic's `bin/optional-modules.py` scans `src/modules/optional/` and
   auto-generates the `OPTIONAL_MODULES_SETUP()` hook that calls `setupCachedPhoneTracker()` — no
   manual registration needed.

4. Flash the firmware. ESP32 also needs the LittleFS image; nRF52840 targets use UF2 / DFU (see
   `DOCUMENTATION.md` for the exact build/flash recipe and Windows toolchain gotchas).

## Storage layout (LittleFS)

| Path | Contents |
|---|---|
| `/tracker_points.dat` | Flat binary log of `TrackPoint` records (16 bytes each, packed) |
| `/tracker_index.dat` | `uint32_t` point count |

`TrackPoint` (16 bytes, packed):

```c
struct TrackPoint {
    uint32_t timestamp;  // Unix epoch seconds
    int32_t  latitudeI;  // degrees * 1e-7
    int32_t  longitudeI; // degrees * 1e-7
    uint16_t altitude;   // metres
    uint8_t  hdop;
    uint8_t  sats;
} __attribute__((packed));
```

## Timestamp note (important)

The phone app displays the position's **`time` field, not `timestamp`**, and on the Heltec
Tracker V2 the GPS "solution time" (`localPosition.timestamp`) is **0/unreliable**. This module
therefore:

- uses **`getTime()`** (the GPS-synced device RTC) as the logged timestamp, and
- sets **both** `pos.time` and `pos.timestamp` when syncing.

See `DOCUMENTATION.md` for the full debugging story.

---

## Links

- [ViVSoft Computers](https://vivsoft.live) — company & projects
- [LoRa Mesh Devices](https://lorameshdevices.com) — ready-to-use LoRa & Meshtastic hardware
