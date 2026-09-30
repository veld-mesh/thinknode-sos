#pragma once
#if defined(MESHTASTIC_SOS_BUTTON)
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

#define SOS_ALARM_GUARD_MS (60 * 1000)

/**
 * Hold-to-confirm SOS button.
 *
 * Hold the user button for SOS_HOLD_MS: accelerating ticks while held, then a loud confirm melody.
 * The SOS goes out as a normal channel text (so phone apps notify) with coordinates + map link,
 * plus a waypoint pin and a position update, and repeats every SOS_REPEAT_MS until cancelled by
 * another hold. Incoming SOS texts from other nodes sound a loud alarm; a short press silences it.
 */
class SosModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    struct Note {
        uint16_t freq; // Hz, 0 = rest
        uint16_t ms;
    };

    SosModule();

    bool isActive() const { return active; }
    // An incoming alarm is sounding or was in the last minute: people tap to silence it
    bool alarmRecently() const { return lastAlarmAt && millis() - lastAlarmAt < SOS_ALARM_GUARD_MS; }

  protected:
    virtual int32_t runOnce() override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

  private:
    // Button state
    uint8_t pin = 0;
    bool wasPressed = false;
    bool firedThisPress = false;
    uint32_t pressStart = 0;
    uint32_t nextTickAt = 0;

    // SOS state
    bool active = false;
    uint32_t repeatCount = 0;
    uint32_t nextRepeatAt = 0;
    uint32_t pendingWaypointAt = 0;
    uint32_t pendingPositionAt = 0;
    bool pendingWaypointRemove = false;

    // Non-blocking melody player
    const Note *melody = nullptr;
    uint8_t melodyLen = 0;
    uint8_t melodyIdx = 0;
    uint8_t melodyLoops = 0;
    uint32_t nextNoteAt = 0;
    bool alarmSounding = false;
    uint32_t lastAlarmAt = 0;
    uint32_t alarmUntil = 0;

    void startSos();
    void cancelSos();
    void sendSosText(bool cancelled);
    void sendWaypoint(bool remove);
    void sendText(const char *text, bool bell);
    void getName(char *buf, size_t len);
    void play(const Note *notes, uint8_t len, uint8_t loops = 1);
    void stopSound();
    bool buzzerAllowed();
    void beep(uint16_t freq, uint16_t ms);
};

extern SosModule *sosModule;
#endif
