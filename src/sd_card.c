#include "sd_card.h"

#include "ff.h"
#include <stdio.h>

static FATFS filesystem;
static bool mounted;

bool sd_card_mount(void) {
    if (mounted) return true;
    FRESULT result = f_mount(&filesystem, "0:", 1);
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
