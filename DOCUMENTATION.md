# CachedPhoneTracker — Full Project Notes

A complete record of what was done to port and extend the **CachedPhoneTracker** Meshtastic
module for the **Heltec Wireless Tracker V2**, including the bugs found and the fixes applied.

---

## 1. Goal

Turn the Heltec Wireless Tracker V2 (ESP32-S3) into a standalone GPS tracker that:

1. Logs GPS positions **offline** to LittleFS every 60 s.
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
| buzzer pins 25/37, `handleMultipress` | removed |

### Added (new work)

- **Status card** — `wantUIFrame()` / `drawFrame()` (the "GPS Tracker" frame).
- **Sub-menu** — `inputObserver` + `handleInputEvent()` (single-button start/stop/clear).
- **`setTrackingEnabled()` / `toggleTracking()`** — runtime toggle (previously missing).
- **Default tracking OFF**, cache cap **500**.

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

### 4.4 Sync flooding + competing with the config/node handshake

The first sync implementation **auto-synced on BLE connect** and sent every cached point
back-to-back with only 50 ms between them. Two problems:

1. The BLE `toPhoneQueue` is small, so sending that fast filled it up and dropped the rest — only
   ~10–20 of 130 entries reached the app.
2. Streaming the cache immediately on connect competed with the app's own config + node-list
   handshake for the same BLE queue, making the app appear to hang ("reading node list" → stuck →
   disconnect). Even a 30 s settle delay wasn't enough.

Final fix:

- **Manual sync only** — sync is now triggered by a **"Send cache"** entry in the on-device menu,
  not automatically on connect. The user connects, waits for the app to finish its handshake, then
  triggers the sync when ready.
- **Pacing** — `TRACKER_SYNC_START_DELAY_MS = 1000` (short settle) and
  `TRACKER_SYNC_INTERVAL_MS = 500` (one entry every 0.5 s) so the queue drains and no points are
  dropped.

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
