#!/usr/bin/env python3
"""
combine_uf2.py – Append Kickstart ROM data to an existing RP2350 UF2 file.

Usage:
    python3 combine_uf2.py firmware.uf2 kickstart.rom combined.uf2

The firmware is written starting at 0x10000000 (flash XIP base).
The ROM is written starting at 0x10200000 (flash offset 0x200000).

This matches the ROM_FLASH_BASE constant in src/memory.h.

ADF floppy images can also be appended at:
  DF0: 0x10280000  (flash offset 0x280000)
  DF1: 0x10480000  (flash offset 0x480000)

    python3 combine_uf2.py firmware.uf2 kickstart.rom combined.uf2 \\
        --adf0 workbench.adf --adf1 extras.adf
"""

import sys
import struct
import argparse

UF2_MAGIC1 = 0x0A324655   # "UF2\n"
UF2_MAGIC2 = 0x9E5D5157
UF2_MAGIC3 = 0x0AB16F30
UF2_FLAG_FAMILYID = 0x00002000
RP2350_FAMILY_ID  = 0xe48bff57  # RP2350 family ID

BLOCK_SIZE   = 512
PAYLOAD_SIZE = 256
FLASH_BASE   = 0x10000000

ROM_FLASH_OFFSET  = 0x200000
ADF0_FLASH_OFFSET = 0x280000
ADF1_FLASH_OFFSET = 0x480000


def read_uf2_blocks(path):
    """Return list of (target_addr, data_256) tuples from a UF2 file."""
    blocks = []
    with open(path, 'rb') as f:
        data = f.read()
    for off in range(0, len(data), BLOCK_SIZE):
        blk = data[off:off + BLOCK_SIZE]
        if len(blk) < BLOCK_SIZE:
            break
        m1, m2, flags, addr, plen, blkno, totalblks, familyid, m3 = \
            struct.unpack_from('<IIIIIIIII', blk, 0)
        if m1 != UF2_MAGIC1 or m2 != UF2_MAGIC2 or m3 != UF2_MAGIC3:
            continue
        payload = blk[32:32 + PAYLOAD_SIZE]
        blocks.append((addr, payload))
    return blocks


def binary_to_uf2_blocks(data, base_addr):
    """Convert raw binary data to a list of (target_addr, data_256) tuples."""
    blocks = []
    for off in range(0, len(data), PAYLOAD_SIZE):
        chunk = data[off:off + PAYLOAD_SIZE]
        chunk = chunk.ljust(PAYLOAD_SIZE, b'\xff')  # pad last block
        blocks.append((base_addr + off, chunk))
    return blocks


def write_uf2(blocks, path):
    """Write (addr, data_256) list to a UF2 file."""
    total = len(blocks)
    with open(path, 'wb') as f:
        for blkno, (addr, payload) in enumerate(blocks):
            hdr = struct.pack('<IIIIIII',
                              UF2_MAGIC1,
                              UF2_MAGIC2,
                              UF2_FLAG_FAMILYID,
                              addr,
                              PAYLOAD_SIZE,
                              blkno,
                              total)
            fam = struct.pack('<I', RP2350_FAMILY_ID)
            blk = hdr + fam + payload + b'\x00' * (BLOCK_SIZE - 32 - PAYLOAD_SIZE - 4)
            blk += struct.pack('<I', UF2_MAGIC3)
            assert len(blk) == BLOCK_SIZE
            f.write(blk)
    print(f"Wrote {total} UF2 blocks to {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('firmware_uf2', help='Firmware UF2 file')
    parser.add_argument('rom_bin',      help='Kickstart ROM binary')
    parser.add_argument('output_uf2',   help='Combined output UF2')
    parser.add_argument('--adf0',       help='DF0 ADF image (optional)')
    parser.add_argument('--adf1',       help='DF1 ADF image (optional)')
    args = parser.parse_args()

    blocks = read_uf2_blocks(args.firmware_uf2)
    print(f"Firmware: {len(blocks)} blocks from {args.firmware_uf2}")

    with open(args.rom_bin, 'rb') as f:
        rom = f.read()
    if rom[0] != 0x11:
        print("WARNING: ROM first byte is not 0x11 – may not be a valid Kickstart ROM")
    # Mirror 256 KB ROM to fill 512 KB slot
    if len(rom) == 0x40000:
        rom = rom * 2
    rom_addr = FLASH_BASE + ROM_FLASH_OFFSET
    rom_blocks = binary_to_uf2_blocks(rom, rom_addr)
    print(f"ROM:      {len(rom_blocks)} blocks at 0x{rom_addr:08x} ({len(rom)//1024} KB)")
    blocks.extend(rom_blocks)

    if args.adf0:
        with open(args.adf0, 'rb') as f:
            adf0 = f.read()
        adf0_addr = FLASH_BASE + ADF0_FLASH_OFFSET
        adf0_blocks = binary_to_uf2_blocks(adf0, adf0_addr)
        print(f"DF0 ADF:  {len(adf0_blocks)} blocks at 0x{adf0_addr:08x}")
        blocks.extend(adf0_blocks)

    if args.adf1:
        with open(args.adf1, 'rb') as f:
            adf1 = f.read()
        adf1_addr = FLASH_BASE + ADF1_FLASH_OFFSET
        adf1_blocks = binary_to_uf2_blocks(adf1, adf1_addr)
        print(f"DF1 ADF:  {len(adf1_blocks)} blocks at 0x{adf1_addr:08x}")
        blocks.extend(adf1_blocks)

    write_uf2(blocks, args.output_uf2)


if __name__ == '__main__':
    main()
