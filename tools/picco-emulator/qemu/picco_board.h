#ifndef HW_MISC_PICCO_BOARD_H
#define HW_MISC_PICCO_BOARD_H

#include "hw/irq.h"
#include "hw/dma/esp_gdma.h"

enum { PICCO_IRQ_GPIO, PICCO_IRQ_SPI2, PICCO_IRQ_I2C0, PICCO_IRQ_I2C1, PICCO_IRQ_USJ, PICCO_IRQ_COUNT };

void picco_board_create(MemoryRegion *sys, qemu_irq *irqs, ESPGdmaState *gdma);

#endif
