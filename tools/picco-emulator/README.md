# Picco emulator (QEMU)

A software emulator of the Boox Picco (ESP32-S3) built on the Espressif QEMU
fork. It boots CrossPoint Reader's `picco` firmware far enough to render the
UI to PNG, so screens can be captured and reviewed without hardware.

## What it models

The board model `qemu/picco_board.c` adds the hardware the base `esp32s3`
machine lacks:

- **GPIO** – buttons (Up=21, Down=9, Confirm=5, Power=2, all active-low), the
  e-ink control pins, and the bit-banged / GPSPI2 panel bus.
- **E-ink panel** – SSD1677/SSD2677 command stream; written frames are dumped
  to PBM (rotate -90 for the portrait view).
- **I2C** – the two controllers plus the touch (TMA525C @0x24), charger
  (SGM41562 @0x03), RTC (PCF8563 @0x51), IMU (QMI8658 @0x6A) and dual-LED
  frontlight (@0x38) chips.
- **SAR ADC** (battery), **USB-Serial-JTAG** console, and GDMA wiring.

It exposes two QMP properties for test drivers: `gpio-low` (a pin-mask of held
buttons) and `touch` (`"x y"` to press, `""` to lift).

## Build

1. Clone the Espressif QEMU fork (tag `esp-develop-9.2.2-20260417`) into a work
   area, e.g. `~/emu/qemu-src`.
2. Copy the board model in and apply the machine patch:
   ```sh
   cp qemu/picco_board.c      qemu-src/hw/misc/
   cp qemu/picco_board.h      qemu-src/include/hw/misc/
   git -C qemu-src apply /path/to/qemu/esp32s3-picco.patch
   ```
   The patch registers the board in `hw/xtensa/esp32s3.c`, adds it to
   `hw/misc/meson.build`, and adds trace hooks in `hw/sd/dwc_sdmmc.c`.
3. Configure and build:
   ```sh
   cd qemu-src && ./configure --target-list=xtensa-softmmu --enable-slirp
   ninja -C build qemu-system-xtensa
   ```
   (needs `libgcrypt20-dev` and `libslirp-dev`).

## Run

`scripts/` drives the emulator. Paths default to `~/emu` (override `EMU`/`FLASH`
/`SD` as needed).

1. Build the `picco` firmware (`pio run -e picco`) from a clean checkout.
2. Assemble a 16 MB flash image (bootloader @0, partition table @0x8000, app
   @0xE0000):
   ```sh
   python3 scripts/mkflash.py bootloader.bin firmware.bin flash.bin
   ```
3. Prepare an **MBR-partitioned** FAT32 SD image (a plain super-floppy image
   will not mount).
4. Boot and capture:
   ```sh
   python3 scripts/shoot.py OUT_DIR STEPS.py
   ```
   `run.sh` launches QEMU with 8 MB **octal** PSRAM
   (`-global driver=ssi_psram,property=is_octal,value=true`), the flash as
   `mtd`, the SD as `sd`, and a QMP socket. `shoot.py` waits for a console
   marker, replays a step list — `('tap', x, y)`, `('key', 'down')`,
   `('wait', s)` — and converts each e-ink frame to a portrait PNG.

## Status / limits

- Our `picco` firmware boots to a fully rendered **Home** screen; input is
  delivered and read correctly (the power button works end-to-end).
- The **stock BOOX firmware cannot run here**: it stops at Onyx's eFuse-HMAC
  device-binding check. Stock screens must come from a physical device.
