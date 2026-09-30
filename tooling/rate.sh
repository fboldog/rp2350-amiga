#!/bin/bash
# Emulated vblanks/s (CIA-A TOD) and shown frames/s (dvi_frames_shown), 10 s.
E=$1; A=$(arm-none-eabi-gdb -batch -ex "p/x (int)&CIAA.tod" $E 2>/dev/null | awk '{print $3}'); F=0x$(arm-none-eabi-nm $E | awk '$3=="dvi_frames_shown"{print $1}')
openocd -s /usr/share/openocd/scripts -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "adapter speed 5000" -c init -c "echo A[capture {mdw $A 1}]" -c "echo C[capture {mdw $F 1}]" -c "sleep 10000" -c "echo B[capture {mdw $A 1}]" -c "echo D[capture {mdw $F 1}]" -c exit 2>&1 | grep -oE "^[ABCD]0x[0-9a-f]+: [0-9a-f]+" | awk '{print $2}' | tr '\n' ' ' | awk '{printf "emulated %.1f vblanks/s, shown %.1f frames/s\n", (strtonum("0x"$3)-strtonum("0x"$1))/10, (strtonum("0x"$4)-strtonum("0x"$2))/10}'
