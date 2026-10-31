// Protocol test of omega/KickSmash.c: drives it as the switcher's transport
// does (amiga/sm_msg_core.c in the KickSmash sources): requests as ROM
// reads, the reply from ROM reads after the trigger, both CRCs checked.
//   (native/regression.sh builds and runs it)
// Exit status 0 when every check passes.

#include <stdio.h>
#include <string.h>
#include "KickSmash.h"
#include "m68k.h"

// The transport runs from RAM; a PC in the ROM must be ignored.
static unsigned test_pc = 0x1000;
unsigned int m68k_get_reg(void *context, m68k_register_t reg) {
    (void)context; (void)reg;
    return test_pc;
}

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, \
                      __LINE__, #c); failures++; } } while (0)

static int settings_changed;
void kickSmashHostSettingsChanged(void) { settings_changed++; }

static uint32_t crc32be(uint32_t crc, const uint8_t *p, unsigned n) {
    while (n--) {
        crc ^= (uint32_t)*p++ << 24;
        for (unsigned b = 0; b < 8; b++)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
    }
    return crc;
}

// A ROM read as the 68000 core makes it: the protocol, else the ROM.
static uint32_t rom_read(uint32_t address) {
    uint32_t v;
    if (kickSmashWatches(address) && kickSmashReadLong(address, &v))
        return v;
    return address == KS_ROM_BASE ? 0x11144ef9u : 0xdeadbeefu;
}

// send_cmd_core(): returns the status, -1 without a reply, -2 on a bad CRC.
static int send(uint16_t cmd, const void *arg, uint16_t arglen,
                uint8_t *rep, unsigned repmax, unsigned *replen,
                unsigned shift) {
    static const uint16_t magic[4] = { 0x0204, 0x1017, 0x0119, 0x0117 };
    uint16_t words[160];
    unsigned n = (arglen + 1u) / 2u;
    const uint8_t *a = arg;
    for (unsigned i = 0; i < n; i++)
        words[i] = (uint16_t)(a[2 * i] << 8 |
                              (2 * i + 1 < arglen ? a[2 * i + 1] : 0));
    for (unsigned i = 0; i < 4; i++)
        rom_read(KS_ROM_BASE + ((uint32_t)magic[i] << shift));
    rom_read(KS_ROM_BASE + ((uint32_t)arglen << shift));
    uint8_t head[4] = { arglen >> 8, arglen, cmd >> 8, cmd };
    uint32_t crc = crc32be(crc32be(0, head, 4), a, arglen);
    rom_read(KS_ROM_BASE + ((uint32_t)cmd << shift));
    for (unsigned i = 0; i < n; i++)
        rom_read(KS_ROM_BASE + ((uint32_t)words[i] << shift));
    if (n & 1)
        rom_read(KS_ROM_BASE + (0xaaaau << shift));
    rom_read(KS_ROM_BASE + ((crc >> 16) << shift));
    rom_read(KS_ROM_BASE + ((crc & 0xffff) << shift));

    unsigned word, got = 0, len = 0, status = 0;
    uint32_t v = 0;
    for (word = 0; word < 128; word++) {
        uint16_t val;
        if (word & 1) {
            val = (uint16_t)v;
        } else {
            v = rom_read(KS_ROM_BASE + 0x1554);
            val = (uint16_t)(v >> 16);
        }
        if (got < 4) {
            got = val == magic[got] ? got + 1 : 0;
            continue;
        }
        if (got == 4) {
            len = val;
            got++;
            continue;
        }
        status = val;
        word++;
        break;
    }
    if (word >= 128)
        return -1;
    uint8_t head2[4] = { len >> 8, len, status >> 8, status };
    crc = crc32be(0, head2, 4);
    uint8_t data[256];
    for (unsigned pos = 0; pos < len; pos += 2, word++) {
        uint16_t val;
        if (word & 1) {
            val = (uint16_t)v;
        } else {
            v = rom_read(KS_ROM_BASE);
            val = (uint16_t)(v >> 16);
        }
        data[pos] = val >> 8;
        data[pos + 1] = (uint8_t)val;
    }
    uint32_t sent = rom_read(KS_ROM_BASE);
    if (crc32be(crc, data, len) != sent)
        return -2;
    if (rep)
        memcpy(rep, data, len < repmax ? len : repmax);
    if (replen)
        *replen = len;
    return (int)status;
}

int main(void) {
    static KickSmashState state;
    kickSmashInit(&state);
    const char *files[] = { "kick13.rom", "kick314.rom", "switcher.rom" };
    kickSmashDefaultBanks(files, 3);
    state.bi.bank_current = 2;

    for (unsigned shift = 1; shift <= 2; shift++) {
        uint8_t rep[256];
        unsigned len;
        CHECK(send(0x01, NULL, 0, rep, sizeof rep, &len, shift) == 0);

        CHECK(send(0x02, NULL, 0, rep, sizeof rep, &len, shift) == 0);
        CHECK(len == 88 && rep[0] == 0 && rep[1] == 2);   // version 2.x
        CHECK(!strcmp((char *)rep + 12, "OMEGA-RP2350"));
        CHECK(rep[60] == (shift == 1));                   // ROM mode

        CHECK(send(0x20, NULL, 0, rep, sizeof rep, &len, shift) == 0);
        CHECK(len == sizeof(KickSmashBankInfo) && len == 160);
        CHECK(rep[0] == 1 && rep[1] == 2);
        CHECK(!strcmp((char *)rep + 20, "kick13"));
        CHECK(!strcmp((char *)rep + 20 + 16, "kick314"));
    }
    CHECK(kickSmashBusy == 0);

    uint8_t rep[64];
    unsigned len;
    // NV: the switcher's timeout and bank
    uint8_t nvset[4] = { 0, 2, 10, 1 };
    settings_changed = 0;
    CHECK(send(0x0207, nvset, 4, NULL, 0, NULL, 2) == 0);
    CHECK(state.nv[0] == 10 && state.nv[1] == 1 && settings_changed == 1);
    uint8_t nvget[2] = { 0, 4 };
    CHECK(send(0x0208, nvget, 2, rep, sizeof rep, &len, 2) == 0);
    CHECK(len == 4 && rep[0] == 10 && rep[1] == 1);

    // Names, power-on bank, long reset sequence, board name
    uint8_t name[2 + 12] = { 0, 1 };
    strcpy((char *)name + 2, "Workbench");
    CHECK(send(0x23, name, 2 + 10, NULL, 0, NULL, 2) == 0);
    CHECK(!strcmp(state.bi.name[1], "Workbench"));
    uint8_t one[2] = { 0, 1 };
    CHECK(send(0x0421, one, 2, NULL, 0, NULL, 2) == 0);     // SETPOWERON
    CHECK(state.bi.bank_poweron == 1);
    CHECK(kickSmashTakeSwitch() == -1);
    uint8_t seq[8] = { 2, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    CHECK(send(0x24, seq, 8, NULL, 0, NULL, 2) == 0);
    CHECK(state.bi.longreset_seq[0] == 2 && state.bi.longreset_seq[2] == 0xff);
    char board[16] = "Test board";
    CHECK(send(0x0107, board, 16, NULL, 0, NULL, 2) == 0);
    CHECK(!strcmp(state.board_name, "Test board"));

    // Errors: unknown command, bad bank, empty bank, bad CRC (reply 0x200)
    CHECK(send(0x55, NULL, 0, NULL, 0, NULL, 2) == 0x0300);
    uint8_t nine[2] = { 0, 9 };
    CHECK(send(0x8121, nine, 2, NULL, 0, NULL, 2) == 0x0400);
    uint8_t five[2] = { 0, 5 };
    CHECK(send(0x8121, five, 2, NULL, 0, NULL, 2) == 0x0400);
    CHECK(kickSmashTakeSwitch() == -1);

    // Reads by the ROM's own code neither start nor feed a request.
    test_pc = 0xF80100;
    CHECK(send(0x01, NULL, 0, NULL, 0, NULL, 2) == -1);
    CHECK(kickSmashBusy == 0);
    test_pc = 0x1000;

    // Switch and Reboot: taken only once the reply has been read.
    uint8_t zero[2] = { 0, 0 };
    CHECK(send(0x8121, zero, 2, NULL, 0, NULL, 2) == 0);    // CURRENT|REBOOT
    CHECK(kickSmashTakeSwitch() == 0);
    CHECK(kickSmashTakeSwitch() == -1);
    // Next reset only
    CHECK(send(0x0221, one, 2, NULL, 0, NULL, 2) == 0);     // SETRESET
    CHECK(kickSmashTakeSwitch() == -1);
    CHECK(kickSmashTakeResetBank() == 1);
    CHECK(kickSmashTakeResetBank() == -1);

    // Merge two banks, then unmerge them
    uint8_t pair[2] = { 1, 0 };     // end 1, start 0
    CHECK(send(0x22, pair, 2, NULL, 0, NULL, 2) == 0);
    CHECK(state.bi.merge[0] == 0x10 && state.bi.merge[1] == 0x11);
    CHECK(send(0x22, pair, 2, NULL, 0, NULL, 2) == 0x0100);  // merged
    CHECK(send(0x0122, pair, 2, NULL, 0, NULL, 2) == 0);
    CHECK(state.bi.merge[0] == 0 && state.bi.merge[1] == 0);

    // Settings round trip
    kickSmashParseConfig("cpu 1 68020\ncpu 9 68020\nrtg 1 on\nrtg 2 off\n"
                         "cycles 1 16\ncycles 2 0\ncycles 0 256\n"
                         "cycles 9 5\n");
    CHECK(kickSmashBankCycles(1) == 16 && !kickSmashBankCycles(2) &&
          !kickSmashBankCycles(0) && !kickSmashBankCycles(-1));
    CHECK(kickSmashBankCpu020(1) && !kickSmashBankCpu020(0));
    CHECK(kickSmashBankRtg(1) && !kickSmashBankRtg(2) &&
          !kickSmashBankRtg(0) && !kickSmashBankRtg(-1));
    char text[2048];
    CHECK(kickSmashFormatConfig(text, sizeof text) > 0);
    static KickSmashState copy;
    kickSmashInit(&copy);
    kickSmashParseConfig(text);
    CHECK(!strcmp(copy.board_name, "Test board"));
    CHECK(copy.bi.bank_poweron == 1);
    CHECK(copy.nv[0] == 10 && copy.nv[1] == 1);
    CHECK(copy.bi.longreset_seq[0] == 2 && copy.bi.longreset_seq[1] == 1 &&
          copy.bi.longreset_seq[2] == 0xff);
    CHECK(!strcmp(copy.file[1], "kick314.rom") &&
          !strcmp(copy.bi.name[1], "Workbench"));
    CHECK(!strcmp(copy.file[2], "switcher.rom") && !copy.file[3][0]);
    CHECK(kickSmashFindBank("SWITCHER.ROM") == 2);
    CHECK(kickSmashBankCpu020(1) && !kickSmashBankCpu020(2));
    CHECK(kickSmashBankRtg(1) && !kickSmashBankRtg(2));
    CHECK(kickSmashBankCycles(1) == 16 && !kickSmashBankCycles(2));
    kickSmashParseConfig("rtg 1 off\n");
    CHECK(!kickSmashBankRtg(1));
    kickSmashParseConfig("cpu 1 68000\n");
    CHECK(!kickSmashBankCpu020(1));

    printf("%s (%d failures)\n", failures ? "FAILED" : "all KickSmash checks passed",
           failures);
    return failures != 0;
}
