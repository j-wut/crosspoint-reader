#pragma once

#include <cstdint>

// Sleep/wake trace written to the wake-up debug log (/logs), so a wake that
// shows the boot splash can be traced on a device without a serial console.

// Where sleep entry had got to. Kept in RTC memory, which survives deep sleep,
// panics, watchdog and software resets, but not a power loss. Values are
// stored in RTC memory: append new ones, never renumber.
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
  DeepSleep = 9,  // handed to HalPowerManager::startDeepSleep()
  BluetoothStopped = 10,
  PluginEventsSent = 11,
  SleepFrameSaved = 12,
  WifiOff = 13,
  TiltOff = 14,
  // Steps inside HalPowerManager::startDeepSleep(), reported by name through
  // halSleepStep().
  InputParked = 15,
  SerialClosed = 16,
  PinsHeld = 17,
  RailsDown = 18,
  WaitRelease = 19,
  WakeArmed = 20,
  EnterSleep = 21,
  RejectedWake = 22,  // a wake that was not held long enough, going back to sleep
  Count
};

// How far setup() got. Shows whether a boot that followed a wake died before
// it finished starting up. Same storage rule as SleepStage.
enum class BootStage : uint8_t {
  None = 0,
  Captured = 1,
  SystemReady = 2,
  GpioReady = 3,
  WakeChecked = 4,
  StorageReady = 5,
  SettingsLoaded = 6,
  WakeDecided = 7,
  BootReported = 8,
  DisplayReady = 9,
  SetupDone = 10,
  FirstLoopDone = 11,
  Count
};

// Call first thing in setup(): reads the reset reason, the previous sleep
// stage and the previous boot's log ring before anything clears them.
void wakeTraceCapture();

// Records how far sleep entry got. Safe to call while the SD card is down.
// While the sleep log is open (see wakeTraceSleepLogBegin) it also logs the
// step with battery and heap.
void wakeTraceStage(SleepStage stage);

// Records how far setup() got.
void wakeTraceBoot(BootStage stage);

// While Debug Logging is on: from here until wakeTraceSleepLogEnd(), every log
// line from any task, plus ESP-IDF driver output, is kept and written to the
// wake-up log at each sleep step. `trigger` names what started the sleep.
void wakeTraceSleepLogBegin(const char* trigger, const char* activity, bool fromTimeout, bool powerOff);

// Writes the kept lines and stops keeping them. Call before the SD card is
// parked; later steps are kept only in RTC memory.
void wakeTraceSleepLogEnd();

// Appends one formatted line to the wake-up debug log (and the serial log).
void appendWakeLog(const char* fmt, ...);

// Writes the boot line, plus the previous boot's last logs when this boot did
// not come from a normal deep-sleep wake. Needs the SD card.
void wakeTraceWriteBootReport(const char* wakeReason, bool stateLoaded, bool showBootScreen, const char* resume,
                              bool panic);
