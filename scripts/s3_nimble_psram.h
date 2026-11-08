// Boards with PSRAM (BOARD_HAS_PSRAM): NimBLE's host (mbuf and HCI pools, connection and GATT structures) allocated
// from PSRAM instead of internal RAM. Measured on an X4 Pro (13/10/2026): the stack takes 46,508 B of internal RAM
// instead of 68,152 B and the radio stays up at Home. The controller (libbtdm) and the task stacks stay in internal RAM
// whatever this says.
//
// Included on the command line (-include), so it lands after the prebuilt sdkconfig.h, which sets
// CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL; the host source is compiled here, so the change takes effect.
// FREEINK_BLE_STACK_IN_PSRAM tells lib/BlePageTurner/src/RadioPolicy.h to use the S3 profile that was
// measured with exactly this setting.
#pragma once
#ifdef BOARD_HAS_PSRAM
#include <sdkconfig.h>

#undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#define CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1
#define FREEINK_BLE_STACK_IN_PSRAM 1
#ifdef FREEINK_DEVICE_PICCO
// BOOX Tiles binding (src/network/TilesBind.cpp) runs Curve25519 and a Wi-Fi
// scan inside GATT write callbacks on the host task.
#undef CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 8192
#endif
#endif
