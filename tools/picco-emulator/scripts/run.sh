#!/bin/bash
# Boot a Picco flash image on the esp32s3 QEMU machine (8 MB octal PSRAM, SD, QMP).
Q=/home/user/emu/qemu-src/build/qemu-system-xtensa
exec $Q -machine esp32s3 -m 8M -L /home/user/emu/qemu-src/pc-bios \
  $PSRAM_OPT \
  -drive file=${FLASH:-/home/user/emu/img/flash.bin},if=mtd,format=raw \
  ${SD:+-drive file=$SD,if=sd,format=raw} \
  ${QMP:+-qmp unix:$QMP,server,nowait} \
  -nographic -serial mon:stdio -no-reboot "$@"
