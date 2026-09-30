#!/bin/bash
TIME=50000000
SHOTS=0
CMD=./native/omega-native
ROM_DIR=sd_card/rom
ADF_DIR=sd_card/adf

declare -A kicks
kicks[kick13.rom]=amiga-os-134-workbench.adf
kicks[kick204.rom]=amiga-os-204-workbench.adf
kicks[kick314.rom]=Install3.2.adf

VIDEO=PAL ./native/build.sh

for kick in "${!kicks[@]}"; do
    printf "starting kick => %s\n" "$kick"
    OMEGA_ROM="$ROM_DIR/$kick" $CMD "" $TIME 0 1> /dev/null
    printf "starting kick => %s with adf => %s\n" "$kick" "${kicks[$kick]}"
    OMEGA_ROM="$ROM_DIR/$kick" $CMD "$ADF_DIR/${kicks[$kick]}" \
        $TIME 0 1> /dev/null
done

exit 0
