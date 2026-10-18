#pragma once

#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "input/InputBroker.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

// -----------------------------------------------------------------------------
// CachedPhoneTracker - Standalone GPS tracking with Meshtastic position sync
// -----------------------------------------------------------------------------
// Logs GPS positions locally (LittleFS) and syncs them to the Meshtastic phone
// app. Three hardware targets:
//   * Heltec Wireless Tracker V2 (ESP32-S3, colour TFT)  — status card + sub-menu
//     driven by the single button (short press cycles, long press selects).
//   * Elecrow ThinkNode M1 (nRF52840, e-ink)             — same screen UI (guarded
//     for e-ink refresh via USE_EINK).
//   * Seeed T1000-E (nRF52840, NO screen)                — button gestures only:
//     1 click = log waypoint, 2 = toggle tracking, 3 = clear cache,
//     4 = GPS toggle, 5 = node status. Auto-syncs on phone connect.
//
// Tracking is OFF by default on every boot (not persisted). Buzzer feedback on
// the screenless T1000-E: ascending/descending tunes for tracking on/off, a low
// "dump" for clear-cache, and a single beep on every saved point while tracking.
// -----------------------------------------------------------------------------

#define MAX_CACHED_POSITIONS 500
// The GPS log interval follows the Meshtastic "position.gps_update_interval"
// setting (configurable in the app: 8 / 20 / 40 / 60 / 80 / 120 s) — see runOnce().
// There is no fixed log timer; the cache logs at the same rate Meshtastic refreshes the GPS.

// Phone-sync pacing: the BLE queue to the app is small, so we pace each entry
// — sending too fast drops entries. Screen builds sync via the "Send cache" menu
// item; the screenless T1000-E auto-syncs on phone connect (see SETTLE below).
#define TRACKER_SYNC_INTERVAL_MS 1000 // delay between each synced entry (app drains slowly)
#define TRACKER_SYNC_SETTLE_MS 5000   // delay after phone connect before auto-sync starts (screenless)

#define CACHE_PATH "/tracker_points.dat"
#define INDEX_PATH "/tracker_index.dat"

// Flat binary log entry (16 bytes, packed)
struct TrackPoint {
    uint32_t timestamp;
    int32_t latitudeI;
    int32_t longitudeI;
    uint16_t altitude;
    uint8_t hdop;
    uint8_t sats;
} __attribute__((packed));

class CachedPhoneTracker : public ProtobufModule<meshtastic_Position>, private concurrency::OSThread
{
    CallbackObserver<CachedPhoneTracker, const InputEvent *> inputObserver =
        CallbackObserver<CachedPhoneTracker, const InputEvent *>(this, &CachedPhoneTracker::handleInputEvent);

  public:
    CachedPhoneTracker();

    bool isTracking() const { return _tracking_enabled; }
    uint32_t getPointCount() const { return _point_count; }

    void setTrackingEnabled(bool on);
    void toggleTracking();
    void clearCache();
    void handleMultipress(int clicks);

#if HAS_SCREEN
    virtual bool wantUIFrame() override;
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

  protected:
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Position *p) override { return false; }
    virtual int32_t runOnce() override;

  private:
    bool _tracking_enabled;
    uint32_t _last_log_time;
    uint32_t _point_count;
    bool _sync_pending;
    uint32_t _sync_index;

    // Screenless (T1000-E) auto-sync-on-connect state
    bool _was_phone_connected = false; // detects the disconnected -> connected transition
    uint32_t _sync_settle_start = 0;   // millis() when the auto-sync settle delay began (0 = disarmed)

    // On-device sub-menu state (single-button navigation)
    bool _menu_active = false;
    uint8_t _menu_index = 0;

    void _logCurrentPosition();
    void _writePoint(TrackPoint *tp);
    void _rotateLog();
    bool _readPoint(uint32_t index, TrackPoint *tp);
    void _saveIndex();
    void _loadIndex();
    int32_t _syncOneEntry();
    bool isPhoneConnected();

    int handleInputEvent(const InputEvent *event);
};

extern CachedPhoneTracker *cachedPhoneTracker;
void setupCachedPhoneTracker();
