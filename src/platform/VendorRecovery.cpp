#include "VendorRecovery.h"

#include <Logging.h>

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

void restartToVendorRecovery() {
  const esp_partition_t* recovery =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "safeboot");
  if (!recovery) {
    LOG_ERR("MAIN", "Vendor recovery partition not found");
    return;
  }
  // Verifies the recovery image first, then erases otadata.
  const esp_err_t err = esp_ota_set_boot_partition(recovery);
  if (err != ESP_OK) {
    LOG_ERR("MAIN", "Selecting vendor recovery failed: %s", esp_err_to_name(err));
    return;
  }
  const esp_partition_t* next = esp_ota_get_boot_partition();
  if (!next || next->address != recovery->address) {
    LOG_ERR("MAIN", "Boot partition is not vendor recovery after selecting it");
    return;
  }
  LOG_INF("MAIN", "Restarting into vendor recovery (%s at 0x%lx)", recovery->label,
          static_cast<unsigned long>(recovery->address));
  delay(50);
  esp_restart();
}

void markRunningAppValid() {
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    LOG_ERR("MAIN", "Marking app valid failed: %s", esp_err_to_name(err));
  } else {
    LOG_INF("MAIN", "Running app marked valid");
  }
}

#else

void restartToVendorRecovery() { LOG_INF("MAIN", "Vendor recovery is not available on this target"); }

void markRunningAppValid() {}

#endif
