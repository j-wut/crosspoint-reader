#include "BooxSd.h"

#include <BoardConfig.h>
#include <BooxExFat.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SD_MMC.h>
#include <driver/sdmmc_host.h>
#include <esp32-hal-periman.h>
#include <esp_vfs_fat.h>
#include <vfs_api.h>

#include <memory>

namespace booxsd {
namespace {

// The stock firmware's mount: SD_MMC.begin("/sdcard", false, false, 40000, 15).
constexpr const char* MOUNT_POINT = "/sdcard";
constexpr int FREQ_KHZ = SDMMC_FREQ_HIGHSPEED;
constexpr uint8_t MAX_FILES = 15;

// SDMMCFS with a begin() that mounts through the exFAT-enabled driver copy.
// beginExFat() is SDMMCFS::begin() from Arduino-ESP32 3.3.11 line for line
// (ESP32-S3 branches only); only the mount and unmount calls differ.
class BooxCardFs final : public fs::SDMMCFS {
 public:
  BooxCardFs() : fs::SDMMCFS(std::make_shared<VFSImpl>()) {}
  sdmmc_card_t* card() const { return _card; }

  bool beginExFat(const char* mountpoint, bool mode1bit, bool format_if_mount_failed, int sdmmc_frequency,
                  uint8_t maxOpenFiles) {
    if (_card) {
      return true;
    }
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_CLK, detachExFat);
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_CMD, detachExFat);
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D0, detachExFat);
    if (!mode1bit) {
      perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D1, detachExFat);
      perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D2, detachExFat);
      perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D3, detachExFat);
    }
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    if (_pin_cmd == -1 || _pin_clk == -1 || _pin_d0 == -1 ||
        (!mode1bit && (_pin_d1 == -1 || _pin_d2 == -1 || _pin_d3 == -1))) {
      LOG_ERR("BOOXSD", "SDMMCFS: some SD pins are not set");
      return false;
    }
    slot_config.clk = static_cast<gpio_num_t>(_pin_clk);
    slot_config.cmd = static_cast<gpio_num_t>(_pin_cmd);
    slot_config.d0 = static_cast<gpio_num_t>(_pin_d0);
    slot_config.d1 = static_cast<gpio_num_t>(_pin_d1);
    slot_config.d2 = static_cast<gpio_num_t>(_pin_d2);
    slot_config.d3 = static_cast<gpio_num_t>(_pin_d3);
    slot_config.width = 4;

    if (!perimanClearPinBus(_pin_cmd)) return false;
    if (!perimanClearPinBus(_pin_clk)) return false;
    if (!perimanClearPinBus(_pin_d0)) return false;
    if (!mode1bit) {
      if (!perimanClearPinBus(_pin_d1)) return false;
      if (!perimanClearPinBus(_pin_d2)) return false;
      if (!perimanClearPinBus(_pin_d3)) return false;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_4BIT;
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = sdmmc_frequency;
    if (mode1bit) {
      host.flags = SDMMC_HOST_FLAG_1BIT;
      slot_config.width = 1;
    }
    _mode1bit = mode1bit;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = format_if_mount_failed;
    mount_config.max_files = maxOpenFiles;
    mount_config.allocation_unit_size = 0;
    mount_config.disk_status_check_enable = false;
    mount_config.use_one_fat = false;

    esp_err_t ret = bxf_esp_vfs_fat_sdmmc_mount(mountpoint, &host, &slot_config, &mount_config, &_card);
    if (ret != ESP_OK) {
      if (ret == ESP_FAIL) {
        LOG_ERR("BOOXSD", "Failed to mount filesystem.");
      } else if (ret == ESP_ERR_INVALID_STATE) {
        _impl->mountpoint(mountpoint);
        LOG_ERR("BOOXSD", "SD Already mounted");
        return true;
      } else {
        LOG_ERR("BOOXSD", "Failed to initialize the card (0x%x).", ret);
      }
      _card = nullptr;
      return false;
    }
    _impl->mountpoint(mountpoint);
    _pdrv = bxf_ff_diskio_get_pdrv_card(_card);

    if (!perimanSetPinBus(_pin_cmd, ESP32_BUS_TYPE_SDMMC_CMD, this, -1, -1) ||
        !perimanSetPinBus(_pin_clk, ESP32_BUS_TYPE_SDMMC_CLK, this, -1, -1) ||
        !perimanSetPinBus(_pin_d0, ESP32_BUS_TYPE_SDMMC_D0, this, -1, -1) ||
        (!mode1bit && (!perimanSetPinBus(_pin_d1, ESP32_BUS_TYPE_SDMMC_D1, this, -1, -1) ||
                       !perimanSetPinBus(_pin_d2, ESP32_BUS_TYPE_SDMMC_D2, this, -1, -1) ||
                       !perimanSetPinBus(_pin_d3, ESP32_BUS_TYPE_SDMMC_D3, this, -1, -1)))) {
      LOG_ERR("BOOXSD", "Failed to set all pins bus to SDMMC");
      detachExFat(this);
      return false;
    }
    return true;
  }

  // SDMMCFS::end() with the exFAT driver's unmount.
  void endExFat() {
    if (!_card) return;
    bxf_esp_vfs_fat_sdcard_unmount(_impl->mountpoint(), _card);
    _impl->mountpoint(nullptr);
    _card = nullptr;
    perimanClearPinBus(_pin_cmd);
    perimanClearPinBus(_pin_clk);
    perimanClearPinBus(_pin_d0);
    if (!_mode1bit) {
      perimanClearPinBus(_pin_d1);
      perimanClearPinBus(_pin_d2);
      perimanClearPinBus(_pin_d3);
    }
  }

 private:
  static bool detachExFat(void* bus) {
    auto* self = static_cast<BooxCardFs*>(bus);
    if (self->_card) self->endExFat();
    return true;
  }
};

BooxCardFs g_card;
bool g_mounted = false;

const char* fsTypeName(const int type) {
  switch (type) {
    case 1:
      return "FAT12";
    case 2:
      return "FAT16";
    case 3:
      return "FAT32";
    case 4:
      return "exFAT";
    default:
      return "none";
  }
}

}  // namespace

bool mount() {
  if (g_mounted) return true;
  const BoardConfig::SdmmcPins& p = BoardConfig::ACTIVE.sdmmc;
  LOG_INF("BOOXSD", "mount: SdFat ready %d, pins clk %d cmd %d d0 %d d1 %d d2 %d d3 %d", Storage.ready() ? 1 : 0,
          p.clk, p.cmd, p.d0, p.d1, p.d2, p.d3);
  // SdFat owns the SDMMC host until it is shut down.
  Storage.prepareForDeepSleep();
  g_card.setPins(p.clk, p.cmd, p.d0, p.d1, p.d2, p.d3);
  const unsigned long start = millis();
  const bool ok = g_card.beginExFat(MOUNT_POINT, false, false, FREQ_KHZ, MAX_FILES);
  g_mounted = ok && g_card.card();
  LOG_INF("BOOXSD", "mount %s after %lu ms", g_mounted ? "ok" : "FAILED", millis() - start);
  logState("after mount");
  return g_mounted;
}

void unmount() {
  if (!g_mounted) return;
  g_card.endExFat();
  g_mounted = false;
  LOG_INF("BOOXSD", "unmounted");
}

bool mounted() { return g_mounted; }
fs::FS& fs() { return g_card; }

bool spaceInfo(uint64_t& total, uint64_t& freeBytes) {
  if (!g_mounted) return false;
  return bxf_esp_vfs_fat_info(MOUNT_POINT, &total, &freeBytes) == ESP_OK;
}

void logState(const char* when) {
  sdmmc_card_t* card = g_card.card();
  if (!g_mounted || !card) {
    LOG_INF("BOOXSD", "%s: not mounted", when);
    return;
  }
  uint64_t total = 0;
  uint64_t freeBytes = 0;
  const esp_err_t infoErr = bxf_esp_vfs_fat_info(MOUNT_POINT, &total, &freeBytes);
  const int type = boox_exfat_fs_type(card);
  LOG_INF("BOOXSD",
          "%s: fs %s, card %s %llu MB, %u-byte sectors, bus %d-bit, host flags 0x%x, freq %d kHz (max %d), "
          "ddr %d, free %llu of %llu MB (%s)",
          when, fsTypeName(type), card->cid.name,
          static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size / (1024 * 1024),
          static_cast<unsigned>(card->csd.sector_size), 1 << card->log_bus_width,
          static_cast<unsigned>(card->host.flags), card->real_freq_khz, card->max_freq_khz, card->is_ddr ? 1 : 0,
          static_cast<unsigned long long>(freeBytes / (1024 * 1024)),
          static_cast<unsigned long long>(total / (1024 * 1024)), esp_err_to_name(infoErr));
}

}  // namespace booxsd
