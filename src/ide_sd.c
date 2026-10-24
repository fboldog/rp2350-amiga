// Board backend of the emulated IDE drive (omega/Gayle.c): the Amiga hard
// disk on the SD card, found by sd_card_find_disk() (a type 0x76 partition
// or a contiguous /hdf/dh0.hdf). Drive sector n is card sector first + n;
// the card's FAT is never written. Without a hard disk there is no drive.

#include "Gayle.h"
#include "psram.h"
#include "sd_card.h"
#include <stdio.h>

static uint32_t disk_first, disk_sectors;
static bool disk_bootable;

void ide_sd_attach(void) {
    if (!sd_card_find_disk(&disk_first, &disk_sectors)) {
        disk_sectors = 0;
        return;
    }
    // Kickstart looks for the RDB in the first 16 sectors; without one
    // (a new, unpartitioned disk) there is nothing to boot from yet.
    uint8_t *sector = gayleDiskBuffer();
    for (uint32_t lba = 0; lba < 16 && lba < disk_sectors; ++lba) {
        if (gayleDiskRead(lba, sector) == 0 && sector[0] == 'R' &&
            sector[1] == 'D' && sector[2] == 'S' && sector[3] == 'K') {
            disk_bootable = true;
            break;
        }
    }
    printf("IDE: %s\n", disk_bootable ? "RDB found" : "no RDB (unpartitioned)");
}

bool ide_sd_bootable(void) {
    return disk_bootable;
}

uint32_t gayleDiskSectors(void) {
    return disk_sectors;
}

uint8_t *gayleDiskBuffer(void) {
    // After the FatFs volume and its sector buffer (sd_card.c).
    return psram_ptr(PSRAM_SD_WORK_OFFSET + 0x600u);
}

int gayleDiskRead(uint32_t lba, uint8_t *buffer) {
    if (lba >= disk_sectors)
        return -1;
    return sd_read_blocks(disk_first + lba, buffer, 1) ? 0 : -1;
}

int gayleDiskWrite(uint32_t lba, const uint8_t *buffer) {
    if (lba >= disk_sectors)
        return -1;
    return sd_write_block(disk_first + lba, buffer) ? 0 : -1;
}
