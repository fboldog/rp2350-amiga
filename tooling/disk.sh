#!/bin/bash
# Select the DF0 disk slot (1-based) for the next boot and reset once, via
# the rotation state in PSRAM (see src/main.c). Rapid repeated SWD resets
# can wedge the chip; recover with target/rp2350-rescue.cfg.
# usage: disk.sh <slot>
n=$(( $1 - 1 ))
openocd -s /usr/share/openocd/scripts -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" -c init \
  -c "mww 0x157ff000 0xadf0b007" -c "mww 0x157ff004 $n" -c "mww 0x157ff008 $(printf '0x%08x' $(( ~n & 0xffffffff )))" \
  -c "reset run" -c exit >/dev/null 2>&1
