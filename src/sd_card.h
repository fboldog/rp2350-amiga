#pragma once

#include <stdbool.h>
#include <stddef.h>

// Mount the first FAT/FAT32 partition on the board's SPI microSD slot.
bool sd_card_mount(void);

// Read a complete file into caller-owned memory. Files larger than capacity
// are rejected; size receives the number of bytes loaded on success.
bool sd_card_load_file(const char *path, void *destination, size_t capacity,
                       size_t *size);
