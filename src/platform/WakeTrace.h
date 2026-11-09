#pragma once

#include <cstdint>

// Sleep/wake trace written to the wake-up debug log (/logs), so a wake that
// shows the boot splash can be traced on a device without a serial console.

// Where enterDeepSleep() had got to. Kept in RTC memory, which survives deep
// sleep, panics, watchdog and software resets, but not a power loss.
enum class SleepStage : uint8_t {
  Awake = 0,
  Started = 1,
  StateSaved = 2,
  ScreenDrawn = 3,
  UsbHold = 4,
  UsbHoldDone = 5,
  DisplayOff = 6,
  StorageParked = 7,
  ShipMode = 8,
  DeepSleep = 9,
};

// Call first thing in setup(): reads the reset reason, the previous sleep
// stage and the previous boot's log ring before anything clears them.
void wakeTraceCapture();

// Records how far sleep entry got. Safe to call while the SD card is down.
void wakeTraceStage(SleepStage stage);

// Appends one formatted line to the wake-up debug log (and the serial log).
void appendWakeLog(const char* fmt, ...);

// Writes the boot line, plus the previous boot's last logs when this boot did
// not come from a normal deep-sleep wake. Needs the SD card.
void wakeTraceWriteBootReport(const char* wakeReason, bool stateLoaded, bool showBootScreen, const char* resume,
                              bool panic);
