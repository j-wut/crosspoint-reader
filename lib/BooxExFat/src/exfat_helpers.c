#include "boox_exfat_rename.h"
#include "idf/ff.h"
#include "idf/diskio_sdmmc.h"

int boox_exfat_fs_type(const sdmmc_card_t* card) {
  const BYTE pdrv = ff_diskio_get_pdrv_card(card);
  if (pdrv == 0xFF) return 0;
  const char drive[3] = {(char)('0' + pdrv), ':', 0};
  FATFS* fs = NULL;
  DWORD freeClusters = 0;
  if (f_getfree(drive, &freeClusters, &fs) != FR_OK || !fs) return 0;
  return fs->fs_type;
}
