# CachedPhoneTracker — Full Project Notes

A complete record of what was done to port and extend the **CachedPhoneTracker** Meshtastic
module for the **Heltec Wireless Tracker V2**, including the bugs found and the fixes applied.

---

## 1. Goal

Turn the Heltec Wireless Tracker V2 (ESP32-S3) into a standalone GPS tracker that:

1. Logs GPS positions **offline** to LittleFS at the Meshtastic **"Position update interval"**
   (`position.gps_update_interval` — 8/20/40/60/80/120 s, configurable in the app).
2. Caches up to **500** points (rolling).
3. **Re-syncs** all cached points to the Meshtastic phone app when it reconnects.
4. Shows an on-device **status card** and a single-button **sub-menu** (start/stop, clear cache).
5. Starts with tracking **OFF** by default.

## 2. Target hardware

| Item | Value |
|---|---|
| Board | Heltec Wireless Tracker V2 |
| MCU | ESP32-S3 |
| Display | ST7735S colour TFT (1.14", SPI) |
| Button | Single user button, `BUTTON_PIN = 0` |
| GPS | u-blox (on-board) |
| Filesystem | LittleFS |
| Meshtastic base commit | `200aa42` |

The module is **cross-platform** and also builds for two **nRF52840** boards:

| Target | Env | Notes |
|---|---|---|
| Elecrow ThinkNode M1 | `thinknode_m1` | e-ink 200×200 (GxEPD2); filesystem open modes (§4.5), e-ink refresh (§4.6) |
| Heltec Mesh Node T096 | `heltec-mesh-node-t096` | ST7735 TFT (TFT_eSPI); builds as-is — no module changes needed |
| Seeed T1000-E | `tracker-t1000-e` | **screenless** — button gestures, auto-sync, buzzer feedback (§4.7) |

## 3. What changed (two files)

Both files live under `src/modules/optional/CachedPhoneTracker/`:

| File | Change |
|---|---|
| `CachedPhoneTracker.h` | Module declaration; `TrackPoint` (16-byte packed record); `MAX_CACHED_POSITIONS = 500`; UI/input method declarations |
| `CachedPhoneTracker.cpp` | Full implementation (ported + extended) |

The module is auto-discovered by Meshtastic's `bin/optional-modules.py`, which generates the
`OPTIONAL_MODULES_SETUP()` hook that calls `setupCachedPhoneTracker()` at module setup time — no
manual registration in `Modules.cpp` is required.

### Ported from the original (nRF52 target)

The original `CachedPhoneTracker` used older Meshtastic APIs. The port updated:

| Old API | Current API |
|---|---|
| `gps->getPosition()` | global `localPosition` |
| `gps->isConnected` | `localPosition` + `getTime()` validity |
| `service.sendPositionToPhone(pos)` | `service->sendToPhone(packet)` |
| `service.isPhoneConnected` | `service->api_state != STATE_DISCONNECTED` |
| `spiLock` | removed (not needed) |
| buzzer pins 25/37 | removed (re-added as `playTones()` melodies for the T1000-E, §4.7) |
| `handleMultipress` | re-added for the T1000-E button gestures (§4.7) |

### Added (new work)

- **Status card** — `wantUIFrame()` / `drawFrame()` (the "GPS Tracker" frame).
- **Sub-menu** — `inputObserver` + `handleInputEvent()` (single-button start/stop/clear).
- **`setTrackingEnabled()` / `toggleTracking()`** — runtime toggle (previously missing).
- **Default tracking OFF**, cache cap **500**.
- **Configurable log interval (v2)** — the log timer was a fixed 60 s; it now follows the
  Meshtastic app's **"Position update interval"** (`position.gps_update_interval`, 8/20/40/60/80/120 s)
  via `Default::getConfiguredOrDefaultMs(...)`, so the cache logs at the same rate the GPS refreshes.
- **nRF52840 support (v3)** — cross-platform file open modes (`CPT_FM_*`) + e-ink menu refresh
  for the ThinkNode M1.
- **T1000-E support (v4)** — screenless compilation (`#if HAS_SCREEN`), 1–5 click button gestures
  via `handleMultipress()`, auto-sync on phone connect, buzzer feedback tunes, and default-OFF
  tracking (see §4.7).

---

## 4. Bugs found and fixed

### 4.1 Single-button event mapping (menu never opened)

The single-button board maps presses differently from a D-pad:

| Press | Event |
|---|---|
| Short | `INPUT_BROKER_USER_PRESS` |
| Long | `INPUT_BROKER_SELECT` |
| Very long | `INPUT_BROKER_SHUTDOWN` (release-triggered) |

The first version listened for `INPUT_BROKER_SELECT_LONG`, which is **never emitted** on a
single-button board, so the menu never opened. Fixed by using `SELECT` (long = open/activate) and
`USER_PRESS` (short = cycle).

### 4.2 App shows "Dec 31, 1969" — the `time` field

The phone app displays the position's **`time` field, not `timestamp`**. The sync code only set
`pos.timestamp`, leaving `pos.time = 0` → the app renders epoch 0 ("Dec 31, 1969"). Fixed by
setting **both**:

```c
pos.timestamp = tp.timestamp;
pos.time      = tp.timestamp; // the app displays `time`
```

### 4.3 App still shows 1969 — the GPS "solution time" is 0

Even after setting `time`, the dates were still wrong. A serial diagnostic (`CPT-DBG`) on the
device showed:

```
ts=0  time=1792281606  getTime=1792281611  q=4  gpsV=1792281611  lat=0 lon=0
```

- `localPosition.timestamp` (the GPS **solution time**) = **0** — unreliable on this board.
- `getTime()` / `getValidTime(RTCQualityGPS)` = **1792281611** (valid, GPS-synced RTC).

The module was keying off the wrong field. Fixed by using **`getTime()`** (the device RTC, which
*is* GPS-synced) as the log timestamp, and guarding on valid time + valid position:

```c
uint32_t now = getTime();
if (now < min_valid_epoch) return;                       // no valid time
if (localPosition.latitude_i == 0 && localPosition.longitude_i == 0) return; // no fix
tp.timestamp = now;
```

`min_valid_epoch = 1700000000` (Nov 2023) is a cheap sanity floor.

---

### 4.4 Sync flooding, handshake competition, and the watchdog reboot

The first sync implementation **auto-synced on BLE connect** and sent every cached point in a
tight loop. Three problems, found in sequence:

1. **Flooding** — sending back-to-back (50 ms apart) filled the BLE `toPhoneQueue` and dropped
   most entries (~20 of 130 reached the app).
2. **Handshake competition** — streaming immediately on connect fought the app's own config +
   node-list handshake for the BLE queue, making the app appear to hang.
3. **Watchdog reboot** — pacing with a blocking `delay()` in a loop (500 ms/1 s × 130 entries)
   blocked the module thread for the whole sync, tripping the ESP32 watchdog → the device hung
   then **rebooted mid-sync** (and only ~20 entries landed before the reset).

Final fix:

- **Manual sync only** — a **"Send cache"** menu item (no auto-sync on connect).
- **Non-blocking sync** — send **one entry per `runOnce()` cycle** and return
  `TRACKER_SYNC_INTERVAL_MS` as the delay to the next entry, instead of looping with `delay()`.
  The thread never blocks long enough to trip the watchdog.
- **Pacing** — `TRACKER_SYNC_INTERVAL_MS = 1000` (one entry every ~1 s; the app drains the BLE
  queue slowly — even 500 ms/entry dropped points).

### 4.5 nRF52 port — `FILE_READ`/`FILE_WRITE`/`FILE_APPEND` don't exist

Compiling for the Elecrow ThinkNode M1 (nRF52840) failed on the Arduino file-mode constants:

```
error: 'FILE_APPEND' was not declared in this scope; did you mean 'LFS_O_APPEND'?
error: 'FILE_READ' was not declared in this scope
error: 'FILE_WRITE' was not declared in this scope
```

The nRF52 filesystem backend is Adafruit LittleFS (`InternalFS`), which only defines
`FILE_O_READ` (=0) and `FILE_O_WRITE` (=1). Crucially, **`FILE_O_WRITE` opens read-write + create +
seeks to the END (it appends — it does *not* truncate** like ESP32's `"w"`).

Fix:

- Added `CPT_FM_READ` / `CPT_FM_WRITE` / `CPT_FM_APPEND` macros (per `ARCH_*`): on nRF52/STM32
  they map to `FILE_O_READ`/`FILE_O_WRITE` (append == `FILE_O_WRITE`); on ESP32/RP2040/Portduino
  they map to `FILE_O_READ`/`FILE_O_WRITE`/`FILE_APPEND`.
- `_saveIndex()` now **removes the index file before writing**, because on nRF52 `FILE_O_WRITE`
  appends and would otherwise grow the index file every call (instead of overwriting it).

`_rotateLog()` already removed the cache file before rewriting, so it was unaffected. The changes
are backwards-compatible with ESP32 (both targets build clean).

### 4.6 e-ink sub-menu doesn't refresh after each keypress (M1)

On the Elecrow ThinkNode M1 (e-ink), navigating the sub-menu looked frozen — the screen had to be
manually refreshed after every short-press. The module already called `screen->forceDisplay()`
after each keypress, but on e-ink that **only marks the panel dirty; it does not re-render the UI
frame**, which waits for the next (slow) frame tick.

Fix — a `refreshTrackerDisplay()` helper, called after every menu keystroke:

```cpp
static void refreshTrackerDisplay()
{
#ifdef USE_EINK
    screen->forceDisplay(true);   // drain command queue + re-render frame + refresh panel
#else
    screen->forceDisplay();       // TFT redraws automatically; plain forceDisplay() is a no-op
#endif
}
```

`screen->forceDisplay(true)` (the `forceUiUpdate` path) adds a `DEMAND_FAST` frame flag, drains the
command queue, forces the UI frame to re-render, and then refreshes the panel. On a TFT
`forceDisplay(true)` would leave the display stuck at a fast framerate (battery drain), so it's
guarded to `USE_EINK`. This is entirely within the module — no Meshtastic source was changed.

### 4.7 T1000-E port (screenless + button gestures + auto-sync)

The Seeed T1000-E (nRF52840) has **no screen** — its Meshtastic build sets
`MESHTASTIC_EXCLUDE_SCREEN=1` (which forces `HAS_SCREEN=0`), so `screen` is unavailable and the
whole on-screen UI (status card + sub-menu) must compile out.

**Screenless compilation.** `wantUIFrame()`, `drawFrame()`, and the screen-gated parts of
`handleInputEvent()` are wrapped in `#if HAS_SCREEN`. On the T1000-E the `#else` branch of
`handleInputEvent()` is a no-op — gestures are handled separately.

**Button gestures.** `handleMultipress(int clicks)` maps click counts to actions:

| Clicks | Action |
|---|---|
| 1 | log a waypoint |
| 2 | toggle tracking |
| 3 | clear cache |
| 4 | GPS toggle (`gps->toggleGpsMode()`) |
| 5 | node status |

The clicks are delivered by Meshtastic's `ButtonThread` (the OneButton library already separates
1 / 2 / 3+ clicks via `attachClick` / `attachDoubleClick` / `attachMultiClick`). Three `#ifdef
TRACKER_T1000_E` hooks in `src/input/ButtonThread.cpp` route each of those cases to
`cachedPhoneTracker->handleMultipress(n)`. This is the **one required Meshtastic source change**
(supplied as `patches/ButtonThread.cpp`); the `#else` branches are the unmodified stock code, so
non-T1000-E targets are unaffected.

**Auto-sync on connect.** A screenless tracker has no "Send cache" menu, so it syncs automatically
when the phone connects. A 5 s settle delay (`TRACKER_SYNC_SETTLE_MS`) lets the app finish reading
config/nodes before the cache streams (the handshake-competition fix from §4.4). Sync remains
non-blocking and paced (one entry per cycle, 1 s apart).

**Buzzer feedback.** The buzzer is the only UI, so each action has a distinct sound via
`playTones()` (forward-declared — no `buzz.h` edit): an ascending arpeggio for tracking ON, a
descending one for OFF, a low "dump" for clear-cache, and a single beep on every saved point while
tracking.

**Default OFF, not persisted.** Tracking starts OFF on every boot. The enabled flag was removed
from the index file (`_saveIndex`/`_loadIndex`), so `_tracking_enabled` is never restored — it only
turns on via the 2-click gesture (or the screen builds' menu).

**Flashing.** The T1000-E uses a UF2 drag-and-drop bootloader (Adafruit family `0xADA52840`), *not*
the nrfutil serial-DFU path — the 1200 bps touch doesn't enter DFU mode on this board. Double-press
reset → the board mounts as a `T1000-E` FAT drive → copy the `.uf2`. (The LoRaWAN *edition* of the
T1000-E uses the Seeed bootloader `0x28860057` and needs the UF2 family patched — a separate
project.)

---

## 5. Build environment (Windows)

All of the following were required to get an ESP32-S3 Meshtastic build working under the
git-bash / MSYS environment on Windows:

1. **Native CPython 3.12** — `<native-python-3.12>`
   (not the MS Store Python, which breaks ESP-IDF).
2. **Recreate `~/.platformio/penv` under Python 3.12** — it had been created under Python 3.13,
   causing an `lfs` ABI mismatch (`lfs.cp313-win_amd64.pyd`).
3. **Patch `idf_tools.py` MSYS check** — in two places the check
   `if 'MSYSTEM' in os.environ:` was changed to `if False:` so the ESP-IDF tools (cmake, ninja,
   xtensa toolchain, esptool, ROM ELFs) would install:
   - `~/.platformio\packages\tool-esp_install\tools\idf_tools.py`
   - `~/.platformio\packages\framework-espidf\tools\idf_tools.py`
4. **`env -u PYTHONPATH`** on every build — the hermes-agent venv leaks onto `sys.path` otherwise.

Build command:

```bash
cd <meshtastic-checkout>
env -u PYTHONPATH <native-python-3.12> \
  -m platformio run -e heltec-wireless-tracker-v2
```

## 6. Flashing (direct esptool)

`pio run -t upload` hangs on this board (auto-reset stalls). Flash directly with esptool instead.

Artifacts (in `.pio/build/heltec-wireless-tracker-v2/`):

| File | Offset | Contents |
|---|---|---|
| `...factory.bin` | `0x0` | bootloader + partitions + app |
| `littlefs-...bin` | `0x670000` | filesystem (per `flasher_args.json` / `default_8MB.csv`) |

```bash
cd .pio/build/heltec-wireless-tracker-v2
penv/Scripts/python.exe -m esptool --port COM4 --baud 921600 --chip esp32s3 \
  write-flash 0x0 firmware-heltec-wireless-tracker-v2-2.8.2.200aa42.factory.bin

penv/Scripts/python.exe -m esptool --port COM4 --baud 921600 --chip esp32s3 \
  write-flash 0x670000 littlefs-heltec-wireless-tracker-v2-2.8.2.200aa42.bin
```

> Flash the LittleFS image when you change the default tracking state or cap, or to clear a stale
> cache (the persisted `tracker_index.dat` otherwise overrides the code default).

The board periodically drops off the USB bus after a failed auto-reset. To get it back into
download mode: **hold BOOT, tap RESET, release BOOT**.

## 7. Testing notes

- The on-device card is reachable by short-pressing through the Meshtastic frame carousel.
- To trigger a re-sync of cached points, disconnect then reconnect the phone (BLE reconnect sets
  `_sync_pending`).
- Timestamps were verified by serial capture of a `LOG_WARN` diagnostic line that dumped
  `localPosition.timestamp`, `localPosition.time`, `getTime()`, and `getRTCQuality()`.
