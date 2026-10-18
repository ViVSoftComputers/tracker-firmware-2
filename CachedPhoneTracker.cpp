#include "CachedPhoneTracker.h"
#include "Default.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "main.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"

// -----------------------------------------------------------------------------
// CachedPhoneTracker - Standalone GPS tracking with Meshtastic position sync
// Ported for Heltec Wireless Tracker V2 (ESP32-S3). No button gestures.
// -----------------------------------------------------------------------------

// Cross-platform file open modes.
//   ESP32 / RP2040 / Portduino: FILE_O_READ="r", FILE_O_WRITE="w" (truncates), FILE_APPEND="a".
//   nRF52 / STM32 (Adafruit LittleFS): only FILE_O_READ(=0) / FILE_O_WRITE(=1) exist, and
//   FILE_O_WRITE opens read-write + create + seeks to END (i.e. it appends — it does NOT truncate).
//   So "append" == FILE_O_WRITE on nRF52, and "overwrite" must be done by removing the file first.
#if defined(ARCH_NRF52) || defined(ARCH_STM32)
#define CPT_FM_READ FILE_O_READ
#define CPT_FM_APPEND FILE_O_WRITE
#define CPT_FM_WRITE FILE_O_WRITE
#else
#define CPT_FM_READ FILE_O_READ
#define CPT_FM_APPEND FILE_APPEND
#define CPT_FM_WRITE FILE_O_WRITE
#endif

static const uint32_t min_valid_epoch = 1700000000; // Nov 2023 guard

CachedPhoneTracker *cachedPhoneTracker;

CachedPhoneTracker::CachedPhoneTracker()
    : ProtobufModule("cached_phone_tracker", meshtastic_PortNum_POSITION_APP, &meshtastic_Position_msg),
      concurrency::OSThread("CachedPhoneTracker"), _tracking_enabled(false), _last_log_time(0), _point_count(0),
      _sync_pending(false), _sync_index(0)
{
    // Restore tracking state + cached point count from disk (no-op on first boot)
    _loadIndex();

    if (inputBroker)
        inputObserver.observe(inputBroker);

    LOG_INFO("CachedPhoneTracker: init. tracking=%s, points=%u\n", _tracking_enabled ? "ON" : "OFF", _point_count);
}

int32_t CachedPhoneTracker::runOnce()
{
    uint32_t now = millis();

    // Sync is MANUAL only — triggered from the on-device menu ("Send cache").
    // We deliberately do NOT auto-sync on connect: streaming the cache while the
    // app is still reading config/nodes floods the BLE queue and can drop the link.
    // The sync is NON-BLOCKING: one entry per runOnce cycle (the returned delay
    // paces the next entry), so the thread never blocks long enough to trip the
    // ESP32 watchdog.
    if (_sync_pending && isPhoneConnected()) {
        return _syncOneEntry();
    }

    if (_tracking_enabled) {
        // Log at the same rate Meshtastic refreshes the GPS — configurable via the
        // app's "Position update interval" (position.gps_update_interval, seconds).
        uint32_t logIntervalMs =
            Default::getConfiguredOrDefaultMs(config.position.gps_update_interval, default_gps_update_interval);
        if (now - _last_log_time >= logIntervalMs) {
            _logCurrentPosition();
            _last_log_time = now;
        }
    }

    return 1000; // Check every second while active
}

void CachedPhoneTracker::_logCurrentPosition()
{
    // Use the device's RTC time (synced to GPS) as the log timestamp. The GPS
    // "solution time" field (localPosition.timestamp) is 0/unreliable on the
    // Heltec Tracker V2, so we can't key off it. getTime() is valid once the
    // RTC has a GPS fix (RTCQualityGPS).
    uint32_t now = getTime();
    if (now < min_valid_epoch) {
        LOG_DEBUG("CachedPhoneTracker: no valid time (RTC not synced), skipping log\n");
        return;
    }

    // Require a real position (not the default 0,0).
    if (localPosition.latitude_i == 0 && localPosition.longitude_i == 0) {
        LOG_DEBUG("CachedPhoneTracker: no valid position, skipping log\n");
        return;
    }

    TrackPoint tp;
    tp.timestamp = now;
    tp.latitudeI = localPosition.latitude_i;
    tp.longitudeI = localPosition.longitude_i;
    tp.altitude = (uint16_t)localPosition.altitude;
    tp.hdop = (uint8_t)localPosition.HDOP;
    tp.sats = (uint8_t)localPosition.sats_in_view;

    _writePoint(&tp);

    LOG_INFO("CachedPhoneTracker: logged point %u, lat=%.6f lon=%.6f\n", _point_count, localPosition.latitude_i * 1e-7,
             localPosition.longitude_i * 1e-7);
}

void CachedPhoneTracker::_writePoint(TrackPoint *tp)
{
    File f = FSCom.open(CACHE_PATH, CPT_FM_APPEND);
    if (!f) {
        LOG_ERROR("CachedPhoneTracker: failed to open points file\n");
        return;
    }

    f.write((uint8_t *)tp, sizeof(TrackPoint));
    f.close();

    _point_count++;

    if (_point_count > MAX_CACHED_POSITIONS) {
        _rotateLog();
    }

    _saveIndex();
}

void CachedPhoneTracker::_rotateLog()
{
    LOG_INFO("CachedPhoneTracker: rotating log (max %u points)\n", MAX_CACHED_POSITIONS);

    // Keep the second half of the log
    uint32_t half = _point_count / 2;
    uint32_t keep = _point_count - half;

    File rf = FSCom.open(CACHE_PATH, CPT_FM_READ);
    if (!rf)
        return;

    rf.seek(half * sizeof(TrackPoint));

    uint8_t *buf = (uint8_t *)malloc(keep * sizeof(TrackPoint));
    if (!buf) {
        rf.close();
        return;
    }

    rf.read(buf, keep * sizeof(TrackPoint));
    rf.close();

    FSCom.remove(CACHE_PATH);
    File wf = FSCom.open(CACHE_PATH, CPT_FM_WRITE);
    if (wf) {
        wf.write(buf, keep * sizeof(TrackPoint));
        wf.close();
    }

    free(buf);
    _point_count = keep;
    _saveIndex();
}

bool CachedPhoneTracker::_readPoint(uint32_t index, TrackPoint *tp)
{
    if (index >= _point_count)
        return false;

    File f = FSCom.open(CACHE_PATH, CPT_FM_READ);
    if (!f)
        return false;

    f.seek(index * sizeof(TrackPoint));
    size_t read = f.read((uint8_t *)tp, sizeof(TrackPoint));
    f.close();

    return read == sizeof(TrackPoint);
}

void CachedPhoneTracker::_saveIndex()
{
    // nRF52's FILE_O_WRITE appends rather than truncates, so remove first to
    // guarantee a fresh, correctly-sized index file on every platform.
    if (FSCom.exists(INDEX_PATH))
        FSCom.remove(INDEX_PATH);

    File f = FSCom.open(INDEX_PATH, CPT_FM_WRITE);
    if (!f)
        return;

    f.write((uint8_t *)&_point_count, sizeof(_point_count));
    f.write((uint8_t *)&_tracking_enabled, sizeof(_tracking_enabled));
    f.close();
}

void CachedPhoneTracker::_loadIndex()
{
    if (!FSCom.exists(INDEX_PATH))
        return;

    File f = FSCom.open(INDEX_PATH, CPT_FM_READ);
    if (!f)
        return;

    f.read((uint8_t *)&_point_count, sizeof(_point_count));
    f.read((uint8_t *)&_tracking_enabled, sizeof(_tracking_enabled));
    f.close();

    // Sanity check
    if (_point_count > MAX_CACHED_POSITIONS) {
        _point_count = 0;
    }
}

bool CachedPhoneTracker::isPhoneConnected()
{
    return service->api_state != MeshService::STATE_DISCONNECTED;
}

int32_t CachedPhoneTracker::_syncOneEntry()
{
    // Start of a new sync?
    if (_sync_index == 0) {
        if (_point_count == 0) {
            _sync_pending = false;
            return 1000;
        }
        LOG_INFO("CachedPhoneTracker: syncing %u cached points to phone\n", _point_count);
    }

    // All done?
    if (_sync_index >= _point_count) {
        LOG_INFO("CachedPhoneTracker: sync complete\n");
        _sync_pending = false;
        _sync_index = 0;
        return 1000;
    }

    // Send exactly one entry, then let runOnce reschedule us for the next.
    TrackPoint tp;
    if (_readPoint(_sync_index, &tp)) {
        meshtastic_Position pos = meshtastic_Position_init_default;
        pos.latitude_i = tp.latitudeI;
        pos.longitude_i = tp.longitudeI;
        pos.altitude = tp.altitude;
        pos.timestamp = tp.timestamp;
        pos.time = tp.timestamp; // the app displays `time`, not `timestamp` — set both
        pos.PDOP = tp.hdop;
        pos.sats_in_view = tp.sats;
        pos.has_latitude_i = true;
        pos.has_longitude_i = true;

        meshtastic_MeshPacket *p = allocDataProtobuf(pos);
        if (p) {
            p->to = NODENUM_BROADCAST;
            p->decoded.want_response = false;
            p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
            service->sendToPhone(p);
        }
    }
    _sync_index++;

    return TRACKER_SYNC_INTERVAL_MS;
}

void CachedPhoneTracker::clearCache()
{
    if (FSCom.exists(CACHE_PATH)) {
        FSCom.remove(CACHE_PATH);
    }

    _point_count = 0;
    _sync_pending = false; // cancel any in-progress sync
    _sync_index = 0;
    _saveIndex();

    LOG_INFO("CachedPhoneTracker: cache cleared\n");
}

void CachedPhoneTracker::setTrackingEnabled(bool on)
{
    if (_tracking_enabled == on)
        return;
    _tracking_enabled = on;
    _last_log_time = millis(); // reset the log timer so a fresh enable logs promptly
    _saveIndex();
    LOG_INFO("CachedPhoneTracker: tracking %s\n", on ? "ON" : "OFF");
}

void CachedPhoneTracker::toggleTracking()
{
    setTrackingEnabled(!_tracking_enabled);
}

bool CachedPhoneTracker::wantUIFrame()
{
    return true;
}

void CachedPhoneTracker::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    graphics::drawCommonHeader(display, x, y, "GPS Tracker");

    if (_menu_active) {
        // Sub-menu: short press cycles the selection, long press activates it.
        static const char *items[] = {"Toggle tracking", "Send cache", "Clear cache", "Back"};
        for (uint8_t i = 0; i < 4; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s %s", (i == _menu_index) ? ">" : " ", items[i]);
            display->drawString(x, graphics::getTextPositions(display)[1 + i], buf);
        }
        return;
    }

    // Status card
    char buf[40];
    snprintf(buf, sizeof(buf), "Tracking: %s", _tracking_enabled ? "ON" : "OFF");
    display->drawString(x, graphics::getTextPositions(display)[1], buf);

    snprintf(buf, sizeof(buf), "Cache: %u/%d", _point_count, MAX_CACHED_POSITIONS);
    display->drawString(x, graphics::getTextPositions(display)[2], buf);
}

int CachedPhoneTracker::handleInputEvent(const InputEvent *event)
{
    if (!screen || !screen->isShowingModuleFrame(this))
        return 0; // Not our frame — let the Screen handle it.

    if (!_menu_active) {
        // Long press (INPUT_BROKER_SELECT on a single-button device) opens the sub-menu.
        if (event->inputEvent == INPUT_BROKER_SELECT) {
            _menu_active = true;
            _menu_index = 0;
            screen->forceDisplay();
            return 1; // consumed
        }
        return 0;
    }

    // Sub-menu is active
    switch (event->inputEvent) {
    case INPUT_BROKER_USER_PRESS:
        // Short press cycles the selection.
        _menu_index = (_menu_index + 1) % 4;
        screen->forceDisplay();
        return 1;
    case INPUT_BROKER_SELECT:
        // Long press activates the highlighted item.
        switch (_menu_index) {
        case 0:
            toggleTracking();
            break;
        case 1:
            // Send cache: request a manual sync (runs in runOnce when the phone is connected).
            _sync_pending = true;
            break;
        case 2:
            clearCache();
            break;
        case 3:
        default:
            break; // Back
        }
        _menu_active = false;
        screen->forceDisplay();
        return 1;
    default:
        return 0;
    }
}

void setupCachedPhoneTracker()
{
    if (!cachedPhoneTracker) {
        cachedPhoneTracker = new CachedPhoneTracker();
    }
}
