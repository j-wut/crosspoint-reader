#pragma once

// Picco: restarts into the vendor recovery app (factory partition "safeboot"),
// the same way the stock firmware does: esp_ota_set_boot_partition() on it,
// which erases otadata so the bootloader falls back to it. On stock firmware,
// Restart in the recovery app selects the main app slot again.
// Returns only if the recovery app is missing or cannot be selected.
void restartToVendorRecovery();

// Picco: confirms the running app, like the stock firmware does at boot. After
// the recovery app re-selects this slot, the bootloader may treat the app as
// pending verification and roll back to recovery on the next reset unless the
// app confirms it. No-op when the app is already confirmed.
void markRunningAppValid();
