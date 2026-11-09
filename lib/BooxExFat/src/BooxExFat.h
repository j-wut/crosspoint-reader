#pragma once

// ESP-IDF's FAT driver (fatfs component, IDF v5.5.5) built a second time with
// exFAT turned on, as in the stock Picco firmware. The framework's copy, used
// by SD_MMC, has exFAT off. All symbols carry a bxf_ prefix.

#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bxf_esp_vfs_fat_sdmmc_mount(const char* base_path, const sdmmc_host_t* host_config,
                                      const void* slot_config, const esp_vfs_fat_mount_config_t* mount_config,
                                      sdmmc_card_t** out_card);
esp_err_t bxf_esp_vfs_fat_sdcard_unmount(const char* base_path, sdmmc_card_t* card);
esp_err_t bxf_esp_vfs_fat_info(const char* base_path, uint64_t* out_total_bytes, uint64_t* out_free_bytes);
BYTE bxf_ff_diskio_get_pdrv_card(const sdmmc_card_t* card);

// FatFs type of the volume on `card` (1 FAT12, 2 FAT16, 3 FAT32, 4 exFAT),
// or 0 when it is not mounted.
int boox_exfat_fs_type(const sdmmc_card_t* card);

#ifdef __cplusplus
}
#endif
