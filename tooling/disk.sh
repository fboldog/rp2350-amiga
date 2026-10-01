#!/bin/bash
# Select the DF0 disk slot (1-based) for the next boot and reset once, via
# the rotation state in PSRAM (see src/main.c). The reset is a watchdog
# reset (PSM WDSEL = everything but the oscillators, then WATCHDOG_CTRL
# TRIGGER), like the RESET button: an OpenOCD "reset run" can leave core 1
# halted by the debugger, so HDMI never starts and core 0 waits forever.
# usage: disk.sh <slot>
n=$(( $1 - 1 ))
openocd -s /usr/share/openocd/scripts -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" -c init \
  -c "mww 0x157ff000 0xadf0b007" -c "mww 0x157ff004 $n" -c "mww 0x157ff008 $(printf '0x%08x' $(( ~n & 0xffffffff )))" \
  -c "mww 0x40018008 0x01fffffc" -c "mww 0x400d8000 0x80000000" -c exit >/dev/null 2>&1
