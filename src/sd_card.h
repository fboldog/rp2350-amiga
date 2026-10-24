#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bring-up record, read over SWD (`sd_status`): how far card init got and
// what the card answered. stage: 1 CMD0 idle, 2 CMD8, 3 ACMD41 ready, 4 OCR,
// 5 initialised, 6 mounted; fail_stage is the stage that failed.
typedef struct {
    uint8_t stage;
    uint8_t fail_stage;
    uint8_t last_r1;
    uint8_t mount_result;
    uint32_t ocr;
} sd_status_t;

extern volatile sd_status_t sd_status;

// Raw 512-byte block access (card sectors), after the card is mounted.
bool sd_read_blocks(uint32_t sector, uint8_t *buffer, unsigned count);
bool sd_write_block(uint32_t sector, const uint8_t *buffer);

// Find the Amiga hard disk: an MBR partition of type 0x76, else a
// contiguous /hdf/dh0.hdf on the FAT partition. Gives its first card
// sector and its size in sectors.
bool sd_card_find_disk(uint32_t *first, uint32_t *sectors);

// Attach that disk as the IDE drive (src/ide_sd.c), after mounting.
// ide_sd_bootable(): it has an RDB, so Kickstart can boot from it.
void ide_sd_attach(void);
bool ide_sd_bootable(void);

// KickSmash ROM banks (src/kicksmash_sd.c), after mounting: init lists
// /rom and reads /rom/kicksmash.cfg (true if there is one); rom_file is the
// build's Kickstart, the power-on bank without a settings file.
bool kicksmash_sd_init(const char *rom_file);
int kicksmash_sd_poweron_bank(void);
bool kicksmash_sd_bank_path(int bank, char *out, size_t size);
void kicksmash_sd_set_current(int bank);   // -1: not a bank
void kicksmash_sd_poll(void);              // saves changed settings

// Mount the first FAT/FAT32 partition on the board's SPI microSD slot.
bool sd_card_mount(void);

// Read a complete file into caller-owned memory. Files larger than capacity
// are rejected; size receives the number of bytes loaded on success.
bool sd_card_load_file(const char *path, void *destination, size_t capacity,
                       size_t *size);
