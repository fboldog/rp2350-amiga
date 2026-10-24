#include "sd_card.h"

#include "ff.h"
#include "psram.h"
#include <stdio.h>

// The volume (with its 512-byte window) and a sector buffer live in PSRAM:
// SRAM is full. The IDE drive's buffer follows them (see ide_sd.c).
#define filesystem (*(FATFS *)psram_ptr(PSRAM_SD_WORK_OFFSET))
#define sector_buffer psram_ptr(PSRAM_SD_WORK_OFFSET + 0x400u)
_Static_assert(sizeof(FATFS) <= 0x400u, "FatFs volume outgrows its PSRAM slot");

#define SD_DISK_PARTITION_TYPE 0x76u    // as Emu68 uses
#define SD_DISK_IMAGE "0:/hdf/dh0.hdf"

volatile sd_status_t sd_status;
static bool mounted;

bool sd_card_mount(void) {
    if (mounted) return true;
    FRESULT result = f_mount(&filesystem, "0:", 1);
    sd_status.mount_result = (uint8_t)result;
    if (result == FR_OK) sd_status.stage = 6;
    if (result != FR_OK) {
        printf("SD: mount failed (%d)\n", (int)result);
        return false;
    }
    mounted = true;
    printf("SD: FAT filesystem mounted\n");
    return true;
}

bool sd_card_load_file(const char *path, void *destination, size_t capacity,
                       size_t *size) {
    FIL file;
    if (!mounted || !path || !destination || !size) return false;

    FRESULT result = f_open(&file, path, FA_READ);
    if (result != FR_OK) {
        printf("SD: cannot open %s (%d)\n", path, (int)result);
        return false;
    }

    FSIZE_t file_size = f_size(&file);
    if (file_size > capacity) {
        printf("SD: %s is too large (%lu bytes)\n", path,
               (unsigned long)file_size);
        f_close(&file);
        return false;
    }

    UINT bytes_read = 0;
    result = f_read(&file, destination, (UINT)file_size, &bytes_read);
    f_close(&file);
    if (result != FR_OK || bytes_read != file_size) {
        printf("SD: read failed for %s (%d, %u/%lu bytes)\n", path,
               (int)result, bytes_read, (unsigned long)file_size);
        return false;
    }

    *size = bytes_read;
    return true;
}

static uint32_t little32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static bool find_partition(uint32_t *first, uint32_t *sectors) {
    const uint8_t *mbr = sector_buffer;
    if (!sd_read_blocks(0, sector_buffer, 1) || mbr[510] != 0x55 ||
        mbr[511] != 0xAA)
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *entry = mbr + 446u + 16u * i;
        if ((entry[0] & 0x7Fu) || entry[4] != SD_DISK_PARTITION_TYPE)
            continue;
        *first = little32(entry + 8);
        *sectors = little32(entry + 12);
        if (*sectors) {
            printf("SD: hard disk is partition %u (%lu MB)\n", i + 1,
                   (unsigned long)(*sectors >> 11));
            return true;
        }
    }
    return false;
}

// The image's sectors are read and written in place, so it must be one run
// of clusters: a fast-seek table with room for one fragment proves it.
static bool find_image(uint32_t *first, uint32_t *sectors) {
    FIL file;
    if (f_open(&file, SD_DISK_IMAGE, FA_READ) != FR_OK) return false;
    DWORD table[4] = { 4 };
    file.cltbl = table;
    FRESULT result = f_lseek(&file, CREATE_LINKMAP);
    const FSIZE_t size = f_size(&file);
    f_close(&file);
    if (result != FR_OK || size < 512u) {
        printf("SD: %s is fragmented, copy it again to a fresh card (%d)\n",
               SD_DISK_IMAGE, (int)result);
        return false;
    }
    *first = (uint32_t)(filesystem.database +
                        (table[2] - 2u) * filesystem.csize);
    *sectors = (uint32_t)(size / 512u);
    printf("SD: hard disk is %s (%lu MB)\n", SD_DISK_IMAGE,
           (unsigned long)(*sectors >> 11));
    return true;
}

bool sd_card_find_disk(uint32_t *first, uint32_t *sectors) {
    if (!mounted || !first || !sectors) return false;
    return find_partition(first, sectors) || find_image(first, sectors);
}
