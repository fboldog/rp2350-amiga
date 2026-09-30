#!/usr/bin/env python3
"""
combine_uf2.py – Append Kickstart ROM data to an existing RP2350 UF2 file.

Usage:
    python3 combine_uf2.py firmware.uf2 kickstart.rom combined.uf2

The firmware is written starting at 0x10000000 (flash XIP base).
The ROM is written starting at 0x10200000 (flash offset 0x200000).

This matches BOARD_ROM_FLASH_BASE in src/board_config.h.

ADF floppy images for DF0 can be appended back to back from 0x10280000
(flash offset 0x280000, 880 KB each, up to 15 on 16 MB of flash). Each boot,
reset included, mounts the next one; power-on starts at the first:

    python3 combine_uf2.py firmware.uf2 kickstart.rom combined.uf2 \\
        --adf workbench.adf --adf demo1.adf --adf demo2.adf

The slot after the last image gets an erased marker, so images left in
flash by an earlier, longer list are not mounted. --adf0 is kept as an
alias for --adf.
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
ADF_SIZE          = 80 * 2 * 11 * 512      # 901,120 bytes
FLASH_SIZE        = 16 * 1024 * 1024
ADF_SLOTS         = (FLASH_SIZE - ADF0_FLASH_OFFSET) // ADF_SIZE


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


def block_family(blk):
    flags = struct.unpack_from('<I', blk, 8)[0]
    if not flags & UF2_FLAG_FAMILYID:
        return None
    return struct.unpack_from('<I', blk, 28)[0]


def renumber(blk, blkno, total):
    """Return blk with its block number and block count replaced."""
    return blk[:20] + struct.pack('<II', blkno, total) + blk[28:]


def write_uf2(firmware_blocks, appended_blocks, family_id, path):
    """Write firmware and appended data as one consistently numbered image."""
    data_blocks = []
    for addr, payload in appended_blocks:
        hdr = struct.pack('<IIIIIII',
                          UF2_MAGIC1,
                          UF2_MAGIC2,
                          UF2_FLAG_FAMILYID,
                          addr,
                          PAYLOAD_SIZE,
                          0,
                          0)
        fam = struct.pack('<I', family_id)
        blk = hdr + fam + payload + b'\x00' * (BLOCK_SIZE - 32 - PAYLOAD_SIZE - 4)
        blk += struct.pack('<I', UF2_MAGIC3)
        assert len(blk) == BLOCK_SIZE
        data_blocks.append(blk)

    # The RP2350 boot ROM takes numBlocks from the first block it accepts and
    # reboots once blocks 0..numBlocks-1 have arrived. Every block of the
    # application family must therefore share one sequence; separately
    # numbered appended data was silently never written. Blocks of other
    # families (e.g. the SDK's RP2350-E10 workaround block) keep their own
    # numbering, and payloads are preserved byte-for-byte.
    blocks = firmware_blocks + data_blocks
    total = sum(1 for blk in blocks if block_family(blk) == family_id)
    blkno = 0
    with open(path, 'wb') as f:
        for blk in blocks:
            if block_family(blk) == family_id:
                blk = renumber(blk, blkno, total)
                blkno += 1
            f.write(blk)
    print(f"Wrote {len(firmware_blocks)} firmware + {len(data_blocks)} data "
          f"blocks to {path} ({total} numbered in family 0x{family_id:08x})")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('firmware_uf2', help='Firmware UF2 file')
    parser.add_argument('rom_bin',      help='Kickstart ROM binary')
    parser.add_argument('output_uf2',   help='Combined output UF2')
    parser.add_argument('--adf', '--adf0', dest='adfs', action='append',
                        default=[], metavar='ADF',
                        help='DF0 ADF image; repeat to rotate between them')
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

    if len(args.adfs) > ADF_SLOTS:
        parser.error(f"at most {ADF_SLOTS} ADF images fit in flash")
    for slot, path in enumerate(args.adfs):
        with open(path, 'rb') as f:
            adf = f.read()
        if len(adf) != ADF_SIZE:
            parser.error(f"{path}: {len(adf)} bytes, expected a {ADF_SIZE}-byte DD ADF")
        addr = FLASH_BASE + ADF0_FLASH_OFFSET + slot * ADF_SIZE
        adf_blocks = binary_to_uf2_blocks(adf, addr)
        print(f"DF0 ADF {slot + 1}: {len(adf_blocks)} blocks at 0x{addr:08x} ({path})")
        blocks.extend(adf_blocks)
    if args.adfs and len(args.adfs) < ADF_SLOTS:
        # End of list: the next slot starts with erased (0xFF) flash.
        end = FLASH_BASE + ADF0_FLASH_OFFSET + len(args.adfs) * ADF_SIZE
        blocks.extend(binary_to_uf2_blocks(b'\xff' * PAYLOAD_SIZE, end))
        print(f"End marker at 0x{end:08x}")

    write_uf2(firmware_blocks, blocks, family_id, args.output_uf2)


if __name__ == '__main__':
    main()
