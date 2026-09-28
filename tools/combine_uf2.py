#!/usr/bin/env python3
"""
combine_uf2.py – Append Kickstart ROM data to an existing RP2350 UF2 file.

Usage:
    python3 combine_uf2.py firmware.uf2 kickstart.rom combined.uf2

The firmware is written starting at 0x10000000 (flash XIP base).
The ROM is written starting at 0x10200000 (flash offset 0x200000).

This matches BOARD_ROM_FLASH_BASE in src/board_config.h.

ADF floppy images can also be appended at:
  DF0: 0x10280000  (flash offset 0x280000)
  DF1: 0x10480000  (flash offset 0x480000)

    python3 combine_uf2.py firmware.uf2 kickstart.rom combined.uf2 \\
        --adf0 workbench.adf --adf1 extras.adf
"""

import struct
import argparse
from collections import Counter

UF2_MAGIC1 = 0x0A324655   # "UF2\n"
UF2_MAGIC2 = 0x9E5D5157
UF2_MAGIC3 = 0x0AB16F30
UF2_FLAG_FAMILYID = 0x00002000
BLOCK_SIZE   = 512
PAYLOAD_SIZE = 256
FLASH_BASE   = 0x10000000

ROM_FLASH_OFFSET  = 0x200000
ADF0_FLASH_OFFSET = 0x280000
ADF1_FLASH_OFFSET = 0x480000


def read_uf2_blocks(path):
    """Read and validate a UF2, returning its original 512-byte blocks."""
    with open(path, 'rb') as f:
        data = f.read()

    if not data or len(data) % BLOCK_SIZE:
        raise ValueError(
            f"{path}: size must be a non-zero multiple of {BLOCK_SIZE} bytes")

    blocks = []
    for off in range(0, len(data), BLOCK_SIZE):
        blk = data[off:off + BLOCK_SIZE]
        m1, m2, flags, addr, plen, blkno, totalblks, familyid = \
            struct.unpack_from('<IIIIIIII', blk, 0)
        m3 = struct.unpack_from('<I', blk, BLOCK_SIZE - 4)[0]
        if m1 != UF2_MAGIC1 or m2 != UF2_MAGIC2 or m3 != UF2_MAGIC3:
            raise ValueError(f"{path}: invalid UF2 magic in block {off // BLOCK_SIZE}")
        if plen == 0 or plen > BLOCK_SIZE - 36:
            raise ValueError(
                f"{path}: invalid payload size {plen} in block {off // BLOCK_SIZE}")
        blocks.append(blk)
    return blocks


def firmware_family_id(blocks):
    """Use the predominant family ID from the input application's UF2 blocks."""
    families = Counter()
    for blk in blocks:
        flags, _addr, _plen, _blkno, _totalblks, familyid = \
            struct.unpack_from('<IIIIII', blk, 8)
        if flags & UF2_FLAG_FAMILYID:
            families[familyid] += 1
    if not families:
        raise ValueError("firmware UF2 contains no family ID")
    return families.most_common(1)[0][0]


def binary_to_uf2_blocks(data, base_addr):
    """Convert raw binary data to a list of (target_addr, data_256) tuples."""
    blocks = []
    for off in range(0, len(data), PAYLOAD_SIZE):
        chunk = data[off:off + PAYLOAD_SIZE]
        chunk = chunk.ljust(PAYLOAD_SIZE, b'\xff')  # pad last block
        blocks.append((base_addr + off, chunk))
    return blocks


def write_uf2(firmware_blocks, appended_blocks, family_id, path):
    """Preserve firmware blocks and append a separately numbered UF2 image."""
    total = len(appended_blocks)
    with open(path, 'wb') as f:
        # RP2350 SDK UF2s can contain metadata/extension blocks.  Keep every
        # firmware block byte-for-byte instead of reconstructing and losing it.
        for blk in firmware_blocks:
            f.write(blk)
        for blkno, (addr, payload) in enumerate(appended_blocks):
            hdr = struct.pack('<IIIIIII',
                              UF2_MAGIC1,
                              UF2_MAGIC2,
                              UF2_FLAG_FAMILYID,
                              addr,
                              PAYLOAD_SIZE,
                              blkno,
                              total)
            fam = struct.pack('<I', family_id)
            blk = hdr + fam + payload + b'\x00' * (BLOCK_SIZE - 32 - PAYLOAD_SIZE - 4)
            blk += struct.pack('<I', UF2_MAGIC3)
            assert len(blk) == BLOCK_SIZE
            f.write(blk)
    print(f"Wrote {len(firmware_blocks)} firmware + {total} data blocks to {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('firmware_uf2', help='Firmware UF2 file')
    parser.add_argument('rom_bin',      help='Kickstart ROM binary')
    parser.add_argument('output_uf2',   help='Combined output UF2')
    parser.add_argument('--adf0',       help='DF0 ADF image (optional)')
    parser.add_argument('--adf1',       help='DF1 ADF image (optional)')
    args = parser.parse_args()

    try:
        firmware_blocks = read_uf2_blocks(args.firmware_uf2)
        family_id = firmware_family_id(firmware_blocks)
    except ValueError as exc:
        parser.error(str(exc))
    print(f"Firmware: {len(firmware_blocks)} blocks from {args.firmware_uf2}")
    print(f"Family ID: 0x{family_id:08x} (copied from firmware)")

    blocks = []

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
        print(f"ADF: {args.adf0}")
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

    write_uf2(firmware_blocks, blocks, family_id, args.output_uf2)


if __name__ == '__main__':
    main()
