// 68020 bitfield instructions on memory (Musashi, omega/m68kop*.c) against a
// plain bit-by-bit model: every instruction, offset and width, with the
// field at every alignment, including fields across five bytes.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "m68k.h"

static uint8_t mem[0x10000];

unsigned int m68k_read_memory_8(unsigned int a) { return mem[a & 0xffff]; }
unsigned int m68k_read_memory_16(unsigned int a) {
    return m68k_read_memory_8(a) << 8 | m68k_read_memory_8(a + 1);
}
unsigned int m68k_read_memory_32(unsigned int a) {
    return m68k_read_memory_16(a) << 16 | m68k_read_memory_16(a + 2);
}
void m68k_write_memory_8(unsigned int a, unsigned int v) { mem[a & 0xffff] = (uint8_t)v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    m68k_write_memory_8(a, v >> 8);
    m68k_write_memory_8(a + 1, v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v);
}
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

void cpu_pulse_reset(void) {}
int cpu_irq_ack(int level) { return level; }
void cpu_instr_callback(void) {}
int disass;

static int get_bit(const uint8_t *m, long bit) {
    return m[bit >> 3] >> (7 - (bit & 7)) & 1;
}
static void set_bit(uint8_t *m, long bit, int v) {
    const uint8_t b = (uint8_t)(0x80 >> (bit & 7));
    m[bit >> 3] = (uint8_t)(v ? m[bit >> 3] | b : m[bit >> 3] & ~b);
}

enum { TST, EXTU, CHG, EXTS, CLR, FFO, SET, INS, OPS };
static const char *const names[OPS] = {
    "bftst", "bfextu", "bfchg", "bfexts", "bfclr", "bfffo", "bfset", "bfins"
};

#define PC   0x400u
#define DATA 0x8000u
#define SLOP 16

static uint32_t next_random(void) {
    static uint32_t x = 0x12345678;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static unsigned failures, checks;

static void check(int op, int offset, int width, int base, int in_reg, uint32_t dn_in) {
    uint8_t want[2 * SLOP + 8];
    for (unsigned i = 0; i < sizeof want; ++i)
        want[i] = (uint8_t)next_random();
    memcpy(&mem[DATA + base - SLOP], want, sizeof want);

    // The model: the field's bits, most significant first.
    const long first = SLOP * 8L + offset;
    uint32_t field = 0;
    for (int i = 0; i < width; ++i)
        field = field << 1 | (uint32_t)get_bit(want, first + i);
    const int n = field >> (width - 1) & 1;
    uint32_t dn_want = dn_in;
    if (op == EXTU)
        dn_want = field;
    else if (op == EXTS)
        dn_want = n && width < 32 ? field | ~0u << width : field;
    else if (op == FFO) {
        int i = 0;
        while (i < width && !(field >> (width - 1 - i) & 1))
            ++i;
        dn_want = (uint32_t)(offset + i);
    }
    for (int i = 0; i < width; ++i) {
        const long b = first + i;
        if (op == CHG)
            set_bit(want, b, !get_bit(want, b));
        else if (op == CLR)
            set_bit(want, b, 0);
        else if (op == SET)
            set_bit(want, b, 1);
        else if (op == INS)
            set_bit(want, b, dn_in >> (width - 1 - i) & 1);
    }
    int n_want = n, z_want = field == 0;
    if (op == INS) {
        const uint32_t ins = width < 32 ? dn_in & ((1u << width) - 1) : dn_in;
        n_want = ins >> (width - 1) & 1;
        z_want = ins == 0;
    }

    // bfxxx (a0){d1:#width}, or {#offset:#width}; the register is d2.
    const uint16_t opword = (uint16_t)(0xe8c0 | op << 8 | 0x10);
    uint16_t word2 = (uint16_t)(2u << 12 | (width & 31));
    word2 |= in_reg ? (uint16_t)(1u << 11 | 1u << 6) : (uint16_t)(offset << 6);
    // At a new address each time: the prefetched words are not reloaded for
    // a PC they already cover.
    const unsigned pc = PC + (checks % 1024u) * 16u;
    m68k_write_memory_16(pc, opword);
    m68k_write_memory_16(pc + 2, word2);
    m68k_write_memory_16(pc + 4, 0x4e71);
    m68k_set_reg(M68K_REG_PC, pc);
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_A0, DATA + base);
    m68k_set_reg(M68K_REG_D1, (uint32_t)offset);
    m68k_set_reg(M68K_REG_D2, dn_in);
    m68k_execute(1);

    const unsigned sr = m68k_get_reg(NULL, M68K_REG_SR);
    const uint32_t dn = m68k_get_reg(NULL, M68K_REG_D2);
    ++checks;
    if (memcmp(&mem[DATA + base - SLOP], want, sizeof want) || dn != dn_want ||
        !!(sr & 8) != n_want || !!(sr & 4) != z_want) {
        if (failures++ < 16)
            printf("%s {%d:%d} base+%d%s: d2=%08x want %08x, NZ=%u%u want %d%d%s\n",
                   names[op], offset, width, base, in_reg ? " (reg)" : "", dn,
                   dn_want, !!(sr & 8), !!(sr & 4), n_want, z_want,
                   memcmp(&mem[DATA + base - SLOP], want, sizeof want)
                       ? " memory differs" : "");
    }
}

int main(void) {
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    m68k_pulse_reset();
    for (int op = 0; op < OPS; ++op)
        for (int width = 1; width <= 32; ++width)
            for (int base = 0; base < 4; ++base) {
                for (int offset = 0; offset < 32; ++offset) {
                    check(op, offset, width, base, 0, next_random());
                    check(op, offset, width, base, 1, next_random());
                }
                for (int offset = -32; offset < 0; ++offset)
                    check(op, offset, width, base, 1, next_random());
            }
    printf("bitfield test: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
