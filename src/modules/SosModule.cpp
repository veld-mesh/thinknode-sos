#include "configuration.h"
#if defined(MESHTASTIC_SOS_BUTTON)
#include "SosModule.h"
#include "Channels.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Router.h"
#include "main.h"
#include "mesh/mesh-pb-constants.h"

#if !defined(ARCH_ESP32) && !defined(ARCH_RP2040) && !defined(ARCH_PORTDUINO)
#include "Tone.h"
#endif

SosModule *sosModule;

#ifndef SOS_HOLD_MS
#define SOS_HOLD_MS 3000 // hold this long to send (or cancel) an SOS
#endif
#ifndef SOS_REPEAT_MS
#define SOS_REPEAT_MS (2 * 60 * 1000) // resend while active
#endif
#define SOS_TICK_START_MS 300 // ticks start after this, so quick clicks stay silent
#define SOS_ALARM_MS (30 * 1000) // how long an incoming SOS alarm sounds unless silenced
#define SOS_WAYPOINT_TTL_SECS (24 * 60 * 60)
#define SOS_ICON 0x1F198 // 🆘

// Small piezo buzzers are loudest near their 2.5-4 kHz resonance, far above the stock UI tones.
#define TONE_HI 3400
#define TONE_MID 2900
#define TONE_LO 2300

static const char SOS_PREFIX[] = "\xF0\x9F\x86\x98"; // 🆘
static const char OK_PREFIX[] = "\xE2\x9C\x85";      // ✅

// Sent: three short high pips then a long tone, twice
static const SosModule::Note confirmMelody[] = {
    {TONE_HI, 120}, {0, 60}, {TONE_HI, 120}, {0, 60}, {TONE_HI, 120}, {0, 60}, {TONE_MID, 600}, {0, 200},
    {TONE_HI, 120}, {0, 60}, {TONE_HI, 120}, {0, 60}, {TONE_HI, 120}, {0, 60}, {TONE_MID, 600},
};
// Cancelled: falling three-note
static const SosModule::Note cancelMelody[] = {{TONE_HI, 200}, {0, 50}, {TONE_MID, 200}, {0, 50}, {TONE_LO, 500}};
// Repeat sent: two quick pips so the sender knows it's still going
static const SosModule::Note repeatMelody[] = {{TONE_HI, 80}, {0, 80}, {TONE_HI, 80}};
// Incoming SOS alarm: siren-ish alternating tones, looped
static const SosModule::Note alarmMelody[] = {{TONE_HI, 250}, {TONE_LO, 250}, {TONE_HI, 250}, {TONE_LO, 250}, {0, 400}};
// Incoming cancel: rising two-note
static const SosModule::Note okMelody[] = {{TONE_LO, 150}, {0, 50}, {TONE_HI, 300}};
// Refused (e.g. public channel)
static const SosModule::Note errorMelody[] = {{TONE_LO, 400}, {0, 100}, {TONE_LO, 400}};

#define LEN(x) (uint8_t)(sizeof(x) / sizeof((x)[0]))

SosModule::SosModule() : SinglePortModule("sos", meshtastic_PortNum_TEXT_MESSAGE_APP), OSThread("SOS")
{
    pin = moduleConfig.detection_sensor.monitor_pin > 0 ? moduleConfig.detection_sensor.monitor_pin : BUTTON_PIN;
    // The user button pin is already configured (with wake sense) by ButtonThread
    if (pin != BUTTON_PIN)
        pinMode(pin, INPUT_PULLUP);
    LOG_INFO("SOS: button on pin %u, hold %u ms, repeat %u s", pin, SOS_HOLD_MS, SOS_REPEAT_MS / 1000);
}

bool SosModule::buzzerAllowed()
{
    return config.device.buzzer_mode != meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
}

void SosModule::beep(uint16_t freq, uint16_t ms)
{
#ifdef PIN_BUZZER
    if (buzzerAllowed())
        tone(PIN_BUZZER, freq, ms);
#endif
}

void SosModule::play(const Note *notes, uint8_t len, uint8_t loops)
{
    melody = notes;
    melodyLen = len;
    melodyIdx = 0;
    melodyLoops = loops;
    nextNoteAt = millis();
}

void SosModule::stopSound()
{
    melody = nullptr;
    alarmSounding = false;
#ifdef PIN_BUZZER
    noTone(PIN_BUZZER);
#endif
}

void SosModule::getName(char *buf, size_t len)
{
    if (moduleConfig.detection_sensor.name[0])
        snprintf(buf, len, "%s", moduleConfig.detection_sensor.name);
    else
        snprintf(buf, len, "SOS %s", owner.short_name);
}

// Formats a 1e-7 degree fixed-point value with 5 decimals (~1 m) without relying on printf float support
static void fmtDeg(char *buf, size_t len, int32_t v)
{
    const char *sign = v < 0 ? "-" : "";
    uint32_t a = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    snprintf(buf, len, "%s%lu.%05lu", sign, (unsigned long)(a / 10000000UL), (unsigned long)((a % 10000000UL) / 100UL));
}

void SosModule::sendText(const char *text, bool bell)
{
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    p->want_ack = false;
    p->priority = meshtastic_MeshPacket_Priority_ALERT;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t n = strnlen(text, sizeof(p->decoded.payload.bytes) - 1);
    memcpy(p->decoded.payload.bytes, text, n);
    if (bell && n < sizeof(p->decoded.payload.bytes))
        p->decoded.payload.bytes[n++] = 7; // bell: triggers alert handling in apps / external notification
    p->decoded.payload.size = n;
    LOG_INFO("SOS: send text id=%u: %.*s", p->id, (int)n, p->decoded.payload.bytes);
    service->sendToMesh(p, RX_SRC_LOCAL, true); // cc to our own phone too
}

void SosModule::sendSosText(bool cancelled)
{
    char name[32], text[200];
    getName(name, sizeof(name));

    if (cancelled) {
        snprintf(text, sizeof(text), "%s %s cancelled - OK now", OK_PREFIX, name);
        sendText(text, false);
        return;
    }

    char head[64];
    if (repeatCount <= 1)
        snprintf(head, sizeof(head), "%s %s!", SOS_PREFIX, name);
    else
        snprintf(head, sizeof(head), "%s %s! (repeat %lu)", SOS_PREFIX, name, (unsigned long)repeatCount);

    int32_t lat = localPosition.latitude_i, lon = localPosition.longitude_i;
    if (lat == 0 && lon == 0) {
        snprintf(text, sizeof(text), "%s No GPS position yet", head);
    } else {
        char la[16], lo[16], age[32] = "";
        fmtDeg(la, sizeof(la), lat);
        fmtDeg(lo, sizeof(lo), lon);
        uint32_t now = getTime();
        if (localPosition.time && now > localPosition.time) {
            uint32_t secs = now - localPosition.time;
            if (secs >= 120)
                snprintf(age, sizeof(age), " (position %lu min old)", (unsigned long)(secs / 60));
        }
        snprintf(text, sizeof(text), "%s %s,%s%s https://maps.google.com/?q=%s,%s", head, la, lo, age, la, lo);
    }
    sendText(text, true);
}

void SosModule::sendWaypoint(bool remove)
{
    int32_t lat = localPosition.latitude_i, lon = localPosition.longitude_i;
    if (!remove && lat == 0 && lon == 0) {
        LOG_WARN("SOS: no position, skip waypoint");
        return;
    }

    meshtastic_Waypoint wp = meshtastic_Waypoint_init_zero;
    wp.id = nodeDB->getNodeNum(); // stable per node, so repeats move the same pin and cancel removes it
    wp.has_latitude_i = true;
    wp.latitude_i = lat;
    wp.has_longitude_i = true;
    wp.longitude_i = lon;
    wp.locked_to = nodeDB->getNodeNum();
    wp.icon = SOS_ICON;
    getName(wp.name, sizeof(wp.name));
    snprintf(wp.description, sizeof(wp.description), "Emergency alert from %s", owner.long_name);
    uint32_t now = getValidTime(RTCQualityFromNet);
    if (remove)
        wp.expire = 1; // already expired: apps drop the pin
    else if (now)
        wp.expire = now + SOS_WAYPOINT_TTL_SECS;

    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = NODENUM_BROADCAST;
    p->channel = 0;
    p->want_ack = false;
    p->priority = meshtastic_MeshPacket_Priority_ALERT;
    p->decoded.portnum = meshtastic_PortNum_WAYPOINT_APP;
    p->decoded.payload.size =
        pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &meshtastic_Waypoint_msg, &wp);
    LOG_INFO("SOS: %s waypoint id=%u", remove ? "remove" : "send", wp.id);
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

void SosModule::startSos()
{
    if (channels.isDefaultChannel(0)) {
        LOG_ERROR("SOS: refusing to send on the default public channel");
        play(errorMelody, LEN(errorMelody));
        return;
    }
    LOG_WARN("SOS: ACTIVATED");
    active = true;
    repeatCount = 1;
    nextRepeatAt = millis() + SOS_REPEAT_MS;
    play(confirmMelody, LEN(confirmMelody));
    sendSosText(false);
    // Space the follow-ups so they don't collide with the text on air
    pendingWaypointRemove = false;
    pendingWaypointAt = millis() + 3000;
    pendingPositionAt = millis() + 6000;
}

void SosModule::cancelSos()
{
    LOG_WARN("SOS: cancelled");
    active = false;
    pendingPositionAt = 0;
    play(cancelMelody, LEN(cancelMelody));
    sendSosText(true);
    pendingWaypointRemove = true;
    pendingWaypointAt = millis() + 3000;
}

ProcessMessage SosModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (isFromUs(&mp) || mp.decoded.payload.size < 3)
        return ProcessMessage::CONTINUE;
    const char *text = (const char *)mp.decoded.payload.bytes;
    size_t n = mp.decoded.payload.size;
    if (n >= 4 && memcmp(text, SOS_PREFIX, 4) == 0) {
        LOG_WARN("SOS: incoming SOS from 0x%x, sounding alarm", mp.from);
        alarmSounding = true;
        alarmUntil = millis() + SOS_ALARM_MS;
        lastAlarmAt = millis();
        play(alarmMelody, LEN(alarmMelody), 255);
    } else if (n >= 3 && memcmp(text, OK_PREFIX, 3) == 0) {
        LOG_INFO("SOS: incoming cancel from 0x%x", mp.from);
        stopSound();
        play(okMelody, LEN(okMelody));
    }
    return ProcessMessage::CONTINUE; // let TextMessageModule etc. handle it too
}

int32_t SosModule::runOnce()
{
    uint32_t now = millis();
    bool pressed = digitalRead(pin) == LOW;

    // --- Button: hold to send / cancel ---
    if (pressed && !wasPressed) {
        pressStart = now;
        firedThisPress = false;
        nextTickAt = now + SOS_TICK_START_MS;
        if (alarmSounding) {
            // Any press silences an incoming alarm (and doesn't count towards an SOS)
            LOG_INFO("SOS: alarm silenced");
            stopSound();
            lastAlarmAt = now; // keep guarding against tap-tap-tap shutdown
            firedThisPress = true;
        }
    }
    if (pressed && !firedThisPress) {
        uint32_t held = now - pressStart;
        if (held >= SOS_HOLD_MS) {
            firedThisPress = true;
            if (active)
                cancelSos();
            else
                startSos();
        } else if (held >= SOS_TICK_START_MS && (int32_t)(now - nextTickAt) >= 0) {
            // Ticks speed up from ~450 ms to ~120 ms apart as the hold approaches the threshold
            uint32_t progress = (held - SOS_TICK_START_MS) * 100 / (SOS_HOLD_MS - SOS_TICK_START_MS);
            uint32_t gap = 450 - progress * 330 / 100;
            beep(active ? TONE_LO : TONE_MID, 40);
            nextTickAt = now + gap;
        }
    }
    wasPressed = pressed;

    // --- Scheduled sends ---
    if (pendingWaypointAt && (int32_t)(now - pendingWaypointAt) >= 0) {
        pendingWaypointAt = 0;
        sendWaypoint(pendingWaypointRemove);
    }
    if (pendingPositionAt && (int32_t)(now - pendingPositionAt) >= 0) {
        pendingPositionAt = 0;
        service->refreshLocalMeshNode();
        service->trySendPosition(NODENUM_BROADCAST, false);
    }
    if (active && (int32_t)(now - nextRepeatAt) >= 0) {
        repeatCount++;
        nextRepeatAt = now + SOS_REPEAT_MS;
        LOG_WARN("SOS: repeat %lu", (unsigned long)repeatCount);
        sendSosText(false);
        pendingWaypointRemove = false;
        pendingWaypointAt = now + 3000;
        pendingPositionAt = now + 6000;
        if (!melody)
            play(repeatMelody, LEN(repeatMelody));
    }

    // --- Melody player ---
    if (alarmSounding && (int32_t)(now - alarmUntil) >= 0)
        stopSound();
    if (melody && (int32_t)(now - nextNoteAt) >= 0) {
        if (melodyIdx >= melodyLen) {
            melodyIdx = 0;
            if (melodyLoops != 255 && --melodyLoops == 0)
                melody = nullptr;
        }
        if (melody) {
            const Note &nt = melody[melodyIdx++];
            if (nt.freq)
                beep(nt.freq, nt.ms);
            nextNoteAt = now + nt.ms;
        }
    }

    return (pressed || melody) ? 20 : 50;
}
#endif
