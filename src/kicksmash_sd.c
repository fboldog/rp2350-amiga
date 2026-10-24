// Board backend of the KickSmash protocol (omega/KickSmash.c), which the
// KickSmash ROM switcher uses: the banks are ROM files in the card's /rom
// directory, sorted by name (up to 8), unless /rom/kicksmash.cfg maps them
// ("bank <n> <file> [name]"); that file also keeps the switcher's settings
// (names, power-on bank, auto-switch timeout, board name) and is written
// when the switcher saves. "Switch and Reboot" loads the bank's file and
// resets the Amiga (main.c). The state lives in PSRAM: SRAM is full.

#include "sd_card.h"

#include "KickSmash.h"
#include "ff.h"
#include "psram.h"
#include "pico/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define KS_STATE ((KickSmashState *)psram_ptr(PSRAM_SD_WORK_OFFSET + 0x800u))
#define KS_TEXT  ((char *)psram_ptr(PSRAM_SD_WORK_OFFSET + 0x1000u))
#define KS_TEXT_SIZE 0x800u
_Static_assert(sizeof(KickSmashState) <= 0x800u,
               "KickSmash state outgrows its PSRAM slot");

#define KS_DIR    "0:/rom"
#define KS_CONFIG "0:/rom/kicksmash.cfg"
#define KS_SAVE_DELAY_US 500000u    // one Save sends several commands

static bool settings_dirty;
static uint32_t settings_changed_us;

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool kicksmash_sd_init(const char *rom_file) {
    KickSmashState *ks = KS_STATE;
    kickSmashInit(ks);

    // The directory's ROM files, sorted, in the text buffer for now.
    char (*names)[KS_FILE_MAX] = (char (*)[KS_FILE_MAX])KS_TEXT;
    const int max = (int)(KS_TEXT_SIZE / KS_FILE_MAX);
    const char *sorted[KS_TEXT_SIZE / KS_FILE_MAX];
    int count = 0;
    DIR dir;
    FILINFO info;
    if (f_opendir(&dir, KS_DIR) == FR_OK) {
        while (count < max && f_readdir(&dir, &info) == FR_OK &&
               info.fname[0]) {
            const size_t n = strlen(info.fname);
            if ((info.fattrib & AM_DIR) || n <= 4 || n >= KS_FILE_MAX ||
                strcasecmp(info.fname + n - 4, ".rom"))
                continue;
            strcpy(names[count], info.fname);
            sorted[count] = names[count];
            count++;
        }
        f_closedir(&dir);
    }
    qsort(sorted, (size_t)count, sizeof sorted[0], by_name);
    kickSmashDefaultBanks(sorted, count);
    int bank = kickSmashFindBank(rom_file);
    ks->bi.bank_poweron = bank >= 0 ? (uint8_t)bank : 0;

    // The settings, over those defaults.
    FIL file;
    UINT got = 0;
    const bool have_config = f_open(&file, KS_CONFIG, FA_READ) == FR_OK;
    if (have_config) {
        f_read(&file, KS_TEXT, KS_TEXT_SIZE - 1, &got);
        f_close(&file);
        KS_TEXT[got] = '\0';
        kickSmashParseConfig(KS_TEXT);
    }
    printf("KickSmash: %d ROM banks%s\n", count > KS_ROM_BANKS ? KS_ROM_BANKS
                                                                : count,
           have_config ? ", " KS_CONFIG : "");
    for (int b = 0; b < KS_ROM_BANKS; b++)
        if (ks->file[b][0])
            printf("  bank %d: %s (%s)%s\n", b, ks->file[b], ks->bi.name[b],
                   b == ks->bi.bank_poweron ? " power-on" : "");
    return have_config;
}

int kicksmash_sd_poweron_bank(void) {
    const KickSmashState *ks = KS_STATE;
    return ks->file[ks->bi.bank_poweron][0] ? ks->bi.bank_poweron : -1;
}

bool kicksmash_sd_bank_path(int bank, char *out, size_t size) {
    const KickSmashState *ks = KS_STATE;
    if (bank < 0 || bank >= KS_ROM_BANKS || !ks->file[bank][0])
        return false;
    snprintf(out, size, KS_DIR "/%s", ks->file[bank]);
    return true;
}

void kicksmash_sd_set_current(int bank) {
    KickSmashState *ks = KS_STATE;
    ks->bi.bank_current = bank >= 0 ? (uint8_t)bank : 0xff;
    ks->bi.bank_nextreset = 0xff;
}

void kickSmashHostSettingsChanged(void) {
    settings_dirty = true;
    settings_changed_us = time_us_32();
}

void kicksmash_sd_poll(void) {
    if (!settings_dirty ||
        time_us_32() - settings_changed_us < KS_SAVE_DELAY_US)
        return;
    settings_dirty = false;
    const int n = kickSmashFormatConfig(KS_TEXT, KS_TEXT_SIZE);
    FIL file;
    UINT written = 0;
    FRESULT result = n > 0 ? f_open(&file, KS_CONFIG,
                                    FA_WRITE | FA_CREATE_ALWAYS)
                           : FR_INVALID_PARAMETER;
    if (result == FR_OK) {
        result = f_write(&file, KS_TEXT, (UINT)n, &written);
        FRESULT closed = f_close(&file);
        if (result == FR_OK)
            result = closed;
    }
    if (result == FR_OK && written == (UINT)n)
        printf("KickSmash: settings saved to " KS_CONFIG "\n");
    else
        printf("KickSmash: cannot save " KS_CONFIG " (%d)\n", (int)result);
}
