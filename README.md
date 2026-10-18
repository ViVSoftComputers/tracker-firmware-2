# CachedPhoneTracker — Meshtastic GPS Tracker Module

A Meshtastic **optional module** that turns a compatible GPS-equipped board (developed on the
**Heltec Wireless Tracker V2**, ESP32-S3; also builds for **nRF52840** boards such as the
**Elecrow ThinkNode M1**) into a standalone GPS tracker with **offline point
caching** and **phone sync**, plus a small on-device status card and sub-menu.

> ⚠️ This is **not** a complete firmware. It is a drop-in Meshtastic module — two source files
> that you copy into an existing Meshtastic source tree. The module is auto-discovered by
> Meshtastic's `optional-modules.py` build hook.

---

## What it does

- Logs the current GPS position to a local **LittleFS** file at the same rate Meshtastic refreshes
  the GPS (the app's **"Position update interval"** = `position.gps_update_interval`: 8/20/40/60/80/120 s)
  while tracking is enabled.
- Caches up to **500** points (`MAX_CACHED_POSITIONS`), rotating out the oldest half when full.
- When you choose **"Send cache"** from the device menu, it **re-syncs every cached point** to the
  phone app — so a track recorded offline isn't lost. Sync is **manual on purpose**: auto-syncing
  on connect floods the BLE queue while the app is still reading config/nodes and can drop the link.
- Sync is **non-blocking and paced** — one entry per loop cycle, ~1 s apart
  (`TRACKER_SYNC_INTERVAL_MS`), so the BLE queue isn't flooded, points aren't dropped, and the
  module thread never blocks long enough to trip the ESP32 watchdog.
- Shows a **status card** on the device screen (`Tracking: ON/OFF`, `Cache: N/500`) and a
  single-button **sub-menu** to toggle tracking, send the cache to the phone, or clear the cache.

## On-device UI (single button)

| Action | Behaviour |
|---|---|
| **Long-press** on the card | Opens the sub-menu |
| **Short-press** | Cycles `Toggle tracking` → `Send cache` → `Clear cache` → `Back` |
| **Long-press** | Activates the highlighted item |

> Single-button boards map **short-press → `INPUT_BROKER_USER_PRESS`** and
> **long-press → `INPUT_BROKER_SELECT`** (not `SELECT_LONG`). A very long hold still powers the
> board off — that is the stock Meshtastic `SHUTDOWN` mapping.

## Defaults

- Tracking starts **OFF** on first boot / after a filesystem reset.
- Cache cap is **500** points (`MAX_CACHED_POSITIONS` in `CachedPhoneTracker.h`).
- Log interval = the app's **"Position update interval"** (`position.gps_update_interval`,
  8/20/40/60/80/120 s; default 120 s). No fixed timer.

## Files

| File | Purpose |
|---|---|
| `CachedPhoneTracker.h` | Module declaration, `TrackPoint` binary record, constants |
| `CachedPhoneTracker.cpp` | Implementation: logging, cache I/O, sync, UI, button input |

## Integrating into Meshtastic

1. Copy the two files into your Meshtastic tree:

   ```
   src/modules/optional/CachedPhoneTracker/CachedPhoneTracker.h
   src/modules/optional/CachedPhoneTracker/CachedPhoneTracker.cpp
   ```

2. Rebuild your target. Meshtastic's `bin/optional-modules.py` scans
   `src/modules/optional/` and auto-generates the `OPTIONAL_MODULES_SETUP()` hook that calls
   `setupCachedPhoneTracker()` — no manual registration needed.

3. Flash the firmware **and** the LittleFS image (see `DOCUMENTATION.md` for the exact
   build/flash recipe and Windows toolchain gotchas).

## Storage layout (LittleFS)

| Path | Contents |
|---|---|
| `/tracker_points.dat` | Flat binary log of `TrackPoint` records (16 bytes each, packed) |
| `/tracker_index.dat` | `uint32_t` point count + `bool` tracking-enabled flag |

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
