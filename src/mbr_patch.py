#!/usr/bin/env python3
"""
mbr_patch.py — Interactive IBM XT-style MBR partition table editor

Written by Claude (Anthropic) and verified by Chris Piker.

Purpose:
  Created to support dual booting FreeDOS and ELKS Linux on IBM XT class
  hardware alongside the mbr88_nasm.asm / mbr88_gas.s boot records.

License:
  MIT License
  Copyright (c) 2025 Chris Piker

  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
  DEALINGS IN THE SOFTWARE.

AI disclosure:
  This code was generated with Anthropic Claude, which is an AI.  It is
  quite understandable that AI generated code may not be suitable for some
  projects.  If you prefer not to depend on AI generated (though human
  verified) code in your open source project, please do not include this
  file.

Acknowledgements:
  Thanks to osdev.org for the reference material and community insights that
  informed the design of the mbr88 boot records this tool supports.
  https://wiki.osdev.org/MBR_(x86)

IBM XT / CHS partition entry layout (16 bytes, all little-endian):
  Byte  0      Status          0x00 = inactive, 0x80 = bootable
  Bytes 1-3    CHS first sector  packed: H, S|((C>>8)<<6), C&0xFF
  Byte  4      Partition type  e.g. 0x01=FAT12, 0x04=FAT16, 0x06=FAT16B
  Bytes 5-7    CHS last sector   same packing as first sector CHS
  Bytes 8-11   LBA start       computed from CHS (little-endian DWORD)
  Bytes 12-15  LBA size        computed from CHS span (little-endian DWORD)
"""

import sys
import os
import shutil
import struct

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

PTABLE_OFFSET  = 0x1BE
ENTRY_SIZE     = 16
BOOTSIG_OFFSET = 0x1FE
LABEL_BASE     = 0x43
LABEL_SLOT_SZ  = 16
LABEL_MAX      = 11

MBR88_SIG        = b'mbr88'
MBR88_SIG_OFFSET = 0x1B9

# Two-column display geometry
COL_WIDTH  = 38     # chars per partition column
COL_GAP    = 4      # chars between columns
LINE_WIDTH = COL_WIDTH * 2 + COL_GAP   # = 80

# ---------------------------------------------------------------------------
# Partition type table
# ---------------------------------------------------------------------------

PART_TYPES = {
    0x00: "Empty",
    0x01: "FAT12",
    0x04: "FAT16 <32MB",
    0x05: "Extended",
    0x06: "FAT16B >=32MB",
    0x0B: "FAT32",
    0x0C: "FAT32 LBA",
    0x80: "MINIX old",
    0x81: "MINIX",
    0x82: "Linux swap",
    0x83: "Linux",
}

# ---------------------------------------------------------------------------
# mbr88 blank template — exact bytes of a compiled blank boot record
# ---------------------------------------------------------------------------

MBR88_TEMPLATE = bytes([
    0xE9, 0x7E, 0x00, 0x42, 0x6F, 0x6F, 0x74, 0x3A, 0x0D, 0x0A, 0x00, 0x41,
    0x20, 0x46, 0x6C, 0x6F, 0x70, 0x70, 0x79, 0x0D, 0x0A, 0x42, 0x20, 0x46,
    0x6C, 0x6F, 0x70, 0x70, 0x79, 0x0D, 0x0A, 0x00, 0x52, 0x65, 0x61, 0x64,
    0x20, 0x65, 0x72, 0x72, 0x6F, 0x72, 0x0D, 0x0A, 0x00, 0x3A, 0x20, 0x4E,
    0x6F, 0x20, 0x62, 0x6F, 0x6F, 0x74, 0x20, 0x72, 0x65, 0x63, 0x6F, 0x72,
    0x64, 0x0D, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFA, 0x31, 0xC0,
    0x8E, 0xD8, 0x8E, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7A, 0xFB, 0xBE, 0x00,
    0x7C, 0xBF, 0x00, 0x7A, 0xB9, 0x00, 0x01, 0xF3, 0xA5, 0xEA, 0x9E, 0x7A,
    0x00, 0x00, 0xBE, 0x03, 0x7A, 0xE8, 0x02, 0x01, 0xE8, 0xB6, 0x00, 0xB0,
    0x3E, 0xCD, 0x10, 0xB4, 0x00, 0xCD, 0x16, 0x3C, 0x61, 0x74, 0x43, 0x3C,
    0x41, 0x74, 0x3F, 0x3C, 0x62, 0x74, 0x3F, 0x3C, 0x42, 0x74, 0x3B, 0x3C,
    0x31, 0x72, 0xE8, 0x3C, 0x34, 0x77, 0xE4, 0x2C, 0x31, 0x88, 0xC3, 0x30,
    0xE4, 0xB1, 0x04, 0xD3, 0xE0, 0x05, 0xBE, 0x7B, 0x89, 0xC6, 0x80, 0x7C,
    0x04, 0x00, 0x74, 0xCF, 0xB4, 0x0E, 0x88, 0xD8, 0x04, 0x31, 0xB7, 0x00,
    0xCD, 0x10, 0xA2, 0x40, 0x7A, 0xB2, 0x80, 0x8A, 0x74, 0x01, 0x8A, 0x4C,
    0x02, 0x8A, 0x6C, 0x03, 0xEB, 0x1E, 0xB2, 0x00, 0xEB, 0x02, 0xB2, 0x01,
    0xB4, 0x0E, 0xB0, 0x41, 0x80, 0xFA, 0x01, 0x75, 0x02, 0xB0, 0x42, 0xB7,
    0x00, 0xCD, 0x10, 0xA2, 0x40, 0x7A, 0xB5, 0x00, 0xB1, 0x01, 0xB6, 0x00,
    0x88, 0xD3, 0xB4, 0x00, 0xCD, 0x13, 0x72, 0x26, 0x88, 0xDA, 0xB4, 0x02,
    0xB0, 0x01, 0xBB, 0x00, 0x7C, 0xCD, 0x13, 0x72, 0x19, 0x80, 0x3E, 0xFE,
    0x7D, 0x55, 0x75, 0x1B, 0x80, 0x3E, 0xFF, 0x7D, 0xAA, 0x75, 0x14, 0xBE,
    0x2A, 0x7A, 0xE8, 0x69, 0x00, 0xEA, 0x00, 0x7C, 0x00, 0x00, 0xBE, 0x20,
    0x7A, 0xE8, 0x5E, 0x00, 0xE9, 0x53, 0xFF, 0xB4, 0x0E, 0xA0, 0x40, 0x7A,
    0xB7, 0x00, 0xCD, 0x10, 0xBE, 0x2D, 0x7A, 0xE8, 0x4C, 0x00, 0xE9, 0x41,
    0xFF, 0xBE, 0x0B, 0x7A, 0xE8, 0x43, 0x00, 0xBE, 0xBE, 0x7B, 0xB3, 0x31,
    0xB9, 0x04, 0x00, 0x80, 0x7C, 0x04, 0x00, 0x74, 0x2D, 0xB4, 0x0E, 0xB7,
    0x00, 0x88, 0xD8, 0xCD, 0x10, 0xB0, 0x20, 0xCD, 0x10, 0x51, 0x88, 0xD8,
    0x2C, 0x31, 0x30, 0xE4, 0xB1, 0x04, 0xD3, 0xE0, 0x05, 0x41, 0x7A, 0x89,
    0xC6, 0xE8, 0x16, 0x00, 0x88, 0xD8, 0x2C, 0x31, 0x30, 0xE4, 0xD3, 0xE0,
    0x05, 0xBE, 0x7B, 0x89, 0xC6, 0x59, 0xFE, 0xC3, 0x83, 0xC6, 0x10, 0xE2,
    0xC6, 0xC3, 0xB4, 0x0E, 0xB7, 0x00, 0xAC, 0x84, 0xC0, 0x74, 0x04, 0xCD,
    0x10, 0xEB, 0xF7, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6D, 0x62, 0x72,
    0x38, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0xAA,
])

# ---------------------------------------------------------------------------
# Session state — globals used freely as per project convention
# ---------------------------------------------------------------------------

mbr           = bytearray(512)  # working copy of the MBR
mbr_path      = ''              # path to the file on disk
file_exists   = False           # was the file present at startup?
has_mbr88_sig = False           # mbr88 signature detected
dirty         = False           # in-memory changes not yet written to disk
geo_heads     = 0               # drive geometry — 0 = not yet set
geo_sectors   = 0

# ---------------------------------------------------------------------------
# CHS / LBA helpers
# ---------------------------------------------------------------------------

def pack_chs(cyl, head, sector):
    """Pack (C, H, S) into the 3-byte MBR CHS format."""
    b0 = head & 0xFF
    b1 = (sector & 0x3F) | ((cyl >> 2) & 0xC0)
    b2 = cyl & 0xFF
    return bytes([b0, b1, b2])


def unpack_chs(raw):
    """Unpack 3-byte MBR CHS field to (cylinder, head, sector)."""
    head   = raw[0]
    sector = raw[1] & 0x3F
    cyl    = ((raw[1] & 0xC0) << 2) | raw[2]
    return cyl, head, sector


def chs_to_lba(cyl, head, sector):
    """Convert CHS to flat LBA using current session geometry."""
    return (cyl * geo_heads + head) * geo_sectors + (sector - 1)

# ---------------------------------------------------------------------------
# Label slot helpers
# ---------------------------------------------------------------------------

def build_label_slot(text):
    """Pack label text into a 16-byte slot: text + CR + LF + null + padding."""
    encoded = text[:LABEL_MAX].encode('ascii', errors='replace')
    slot    = encoded + b'\r\n\x00'
    return (slot + b'\x00' * (LABEL_SLOT_SZ - len(slot)))[:LABEL_SLOT_SZ]


def read_label(slot_0based):
    """Read the label string for a 0-based slot from the global mbr buffer."""
    off  = LABEL_BASE + slot_0based * LABEL_SLOT_SZ
    raw  = mbr[off:off + LABEL_MAX]
    null = raw.find(0)
    text = raw[:null] if null >= 0 else raw
    return text.decode('ascii', errors='replace').rstrip()


def write_label(slot_0based, text):
    """Write a label string into the global mbr buffer."""
    off = LABEL_BASE + slot_0based * LABEL_SLOT_SZ
    mbr[off:off + LABEL_SLOT_SZ] = build_label_slot(text)

# ---------------------------------------------------------------------------
# mbr88 detection and upgrade
# ---------------------------------------------------------------------------

def detect_mbr88():
    """Return True if the mbr88 signature is present at offset 0x1B9."""
    return mbr[MBR88_SIG_OFFSET:MBR88_SIG_OFFSET + 5] == MBR88_SIG


def upgrade_to_mbr88():
    """Replace boot code with the blank mbr88 template, preserve ptable."""
    old_ptable = bytes(mbr[PTABLE_OFFSET:PTABLE_OFFSET + 64])
    mbr[:] = bytearray(MBR88_TEMPLATE)
    mbr[PTABLE_OFFSET:PTABLE_OFFSET + 64] = old_ptable

# ---------------------------------------------------------------------------
# Input helpers
# ---------------------------------------------------------------------------

def ask_int(prompt, lo, hi):
    """Prompt until the user enters an integer in [lo, hi]."""
    while True:
        raw = input(prompt).strip()
        try:
            val = int(raw)
        except ValueError:
            print(f"  Please enter a whole number between {lo} and {hi}.")
            continue
        if lo <= val <= hi:
            return val
        print(f"  Value must be between {lo} and {hi}.")


def ask_hex(prompt, lo, hi):
    """Prompt for a hex or decimal integer in [lo, hi]."""
    while True:
        raw = input(prompt).strip()
        try:
            val = int(raw, 0)
        except ValueError:
            print(f"  Please enter hex (0x..) or decimal between {lo} and {hi}.")
            continue
        if lo <= val <= hi:
            return val
        print(f"  Value must be between 0x{lo:02X} and 0x{hi:02X}.")


def ask_yn(prompt):
    """Prompt for Y/y or N/n. Returns True for yes."""
    while True:
        ch = input(prompt).strip().lower()
        if ch == 'y': return True
        if ch == 'n': return False
        print("  Please press Y or N.")

# ---------------------------------------------------------------------------
# Two-column partition table display
# ---------------------------------------------------------------------------

def _is_empty(slot_0based):
    """Return True if the given 0-based partition slot is all zeros."""
    off = PTABLE_OFFSET + slot_0based * ENTRY_SIZE
    e   = mbr[off:off + ENTRY_SIZE]
    return (e[4] == 0
            and all(b == 0 for b in e[1:4])
            and all(b == 0 for b in e[5:8]))


def _size_str(lba_size):
    """Format a sector count as a human-readable size string."""
    b = lba_size * 512
    if b >= 1024 * 1024:
        return f"{b // (1024 * 1024)} MB"
    if b >= 1024:
        return f"{b // 1024} KB"
    return f"{b} B"


def _col_lines(slot_1based):
    """
    Return a list of COL_WIDTH-wide strings for one partition column.
    Always returns exactly 7 lines so both columns zip cleanly.
    """
    i   = slot_1based - 1
    lines = []

    def row(s):
        lines.append(s[:COL_WIDTH].ljust(COL_WIDTH))

    if _is_empty(i):
        row(f" Partition {slot_1based}")
        row("  <empty>")
        for _ in range(5):
            row("")
        return lines

    off  = PTABLE_OFFSET + i * ENTRY_SIZE
    e    = mbr[off:off + ENTRY_SIZE]

    status    = e[0]
    part_type = e[4]
    cyl_s, head_s, sec_s = unpack_chs(e[1:4])
    cyl_e, head_e, sec_e = unpack_chs(e[5:8])
    lba_size  = struct.unpack_from('<I', e, 12)[0]

    boot_tag  = "Boot" if status == 0x80 else "    "
    type_name = PART_TYPES.get(part_type, "Unknown")

    row(f" Partition {slot_1based}  [{boot_tag}]")
    row(f"  Type:  0x{part_type:02X} {type_name[:18]}")
    if has_mbr88_sig:
        label = read_label(i)
        row(f"  Label: {label[:28]}")
    else:
        row("")
    row(f"  Start: C={cyl_s:<4} H={head_s:<3} S={sec_s}")
    row(f"  End:   C={cyl_e:<4} H={head_e:<3} S={sec_e}")
    row(f"  Size:  {_size_str(lba_size)} ({lba_size} sectors)")
    return lines


def print_table():
    """Print the full two-column partition table plus status bar."""
    gap = ' ' * COL_GAP

    print('=' * LINE_WIDTH)
    title = "MBR Partition Table"
    if dirty:
        title += "  (* unsaved changes)"
    print(title.center(LINE_WIDTH))
    print('=' * LINE_WIDTH)

    # Top row: partitions 1 (left) and 2 (right)
    for l, r in zip(_col_lines(1), _col_lines(2)):
        print(l + gap + r)

    print('-' * LINE_WIDTH)

    # Bottom row: partitions 3 (left) and 4 (right)
    for l, r in zip(_col_lines(3), _col_lines(4)):
        print(l + gap + r)

    print('=' * LINE_WIDTH)

    # Status bar — file, signature type, geometry
    sig_tag = "[mbr88]" if has_mbr88_sig else "[generic MBR]"
    geo_tag = (f"Geometry: {geo_heads}H/{geo_sectors}S"
               if geo_heads else "Geometry:")
    print(f"  File: {os.path.basename(mbr_path)}  {sig_tag}  {geo_tag}")
    print()

# ---------------------------------------------------------------------------
# Command implementations
# ---------------------------------------------------------------------------

def cmd_geometry():
    """g — set drive geometry (required before 'n')."""
    global geo_heads, geo_sectors
    print()
    print("Drive geometry tells the tool how to convert CHS addresses into")
    print("LBA sector numbers for the partition table entries.")
    print()
    print("IBM XT default: 16 heads per cylinder, 17 sectors per track.")
    print("Check your drive specification or use values reported by fdisk.")
    print()
    if ask_yn("Use IBM XT default geometry (16H / 17S)? (Y/N): "):
        geo_heads   = 16
        geo_sectors = 17
    else:
        geo_heads   = ask_int("  Heads per cylinder (1-255): ", 1, 255)
        geo_sectors = ask_int("  Sectors per track  (1-63):  ", 1, 63)
    print(f"  Geometry set: {geo_heads} heads, {geo_sectors} sectors/track.")


def _ask_slot(prompt):
    """Prompt for a partition number 1-4. Returns int or None."""
    raw = input(prompt).strip()
    if raw in ('1', '2', '3', '4'):
        return int(raw)
    print("  Please enter a partition number 1-4.")
    return None


def cmd_new():
    """n — define a new partition or fully redefine an existing one."""
    global dirty
    if not geo_heads:
        print("  Geometry not set — run 'g' first.")
        return

    slot = _ask_slot("  Partition number (1-4): ")
    if slot is None:
        return

    i   = slot - 1
    off = PTABLE_OFFSET + i * ENTRY_SIZE

    if not _is_empty(i):
        if not ask_yn(f"  Partition {slot} is not empty. Redefine it? (Y/N): "):
            return

    print(f"\n  -- Partition {slot}: Starting CHS --")
    cyl_s  = ask_int("    Cylinder (0-1023): ", 0, 1023)
    head_s = ask_int("    Head     (0-255):  ", 0, 255)
    sec_s  = ask_int("    Sector   (1-63):   ", 1, 63)

    print(f"\n  -- Partition {slot}: Ending CHS --")
    cyl_e  = ask_int("    Cylinder (0-1023): ", 0, 1023)
    head_e = ask_int("    Head     (0-255):  ", 0, 255)
    sec_e  = ask_int("    Sector   (1-63):   ", 1, 63)

    print("\n  Common partition types:")
    for code, name in PART_TYPES.items():
        print(f"    0x{code:02X}  {name}")
    part_type = ask_hex("  Partition type (hex or decimal): ", 0x00, 0xFF)

    bootable = ask_yn("  Mark as bootable? (Y/N): ")

    lba_start = chs_to_lba(cyl_s, head_s, sec_s)
    lba_end   = chs_to_lba(cyl_e, head_e, sec_e)
    lba_size  = max(0, lba_end - lba_start + 1)

    entry = bytearray(ENTRY_SIZE)
    entry[0]    = 0x80 if bootable else 0x00
    entry[1:4]  = pack_chs(cyl_s, head_s, sec_s)
    entry[4]    = part_type
    entry[5:8]  = pack_chs(cyl_e, head_e, sec_e)
    entry[8:12] = struct.pack('<I', lba_start)
    entry[12:16]= struct.pack('<I', lba_size)

    mbr[off:off + ENTRY_SIZE] = entry
    dirty = True
    print(f"  Partition {slot} defined.")


def cmd_delete():
    """d — delete (zero) a partition slot and its label."""
    global dirty
    slot = _ask_slot("  Partition number to delete (1-4): ")
    if slot is None:
        return

    if _is_empty(slot - 1):
        print(f"  Partition {slot} is already empty.")
        return

    if not ask_yn(f"  Delete partition {slot}?  This zeros all 16 bytes "
                  f"of the entry and its label. (Y/N): "):
        return

    off = PTABLE_OFFSET + (slot - 1) * ENTRY_SIZE
    mbr[off:off + ENTRY_SIZE] = bytes(ENTRY_SIZE)

    if has_mbr88_sig:
        lab_off = LABEL_BASE + (slot - 1) * LABEL_SLOT_SZ
        mbr[lab_off:lab_off + LABEL_SLOT_SZ] = bytes(LABEL_SLOT_SZ)

    dirty = True
    print(f"  Partition {slot} deleted.")


def cmd_set_type():
    """s — change the partition type byte of an existing entry."""
    global dirty
    slot = _ask_slot("  Partition number (1-4): ")
    if slot is None:
        return

    if _is_empty(slot - 1):
        print(f"  Partition {slot} is empty — use 'n' to define it first.")
        return

    print("\n  Common partition types:")
    for code, name in PART_TYPES.items():
        print(f"    0x{code:02X}  {name}")
    part_type = ask_hex("  New partition type (hex or decimal): ", 0x00, 0xFF)

    mbr[PTABLE_OFFSET + (slot - 1) * ENTRY_SIZE + 4] = part_type
    dirty = True
    print(f"  Partition {slot} type set to 0x{part_type:02X}.")


def cmd_bootable():
    """b — toggle the bootable flag (0x00 / 0x80) on an existing entry."""
    global dirty
    slot = _ask_slot("  Partition number (1-4): ")
    if slot is None:
        return

    if _is_empty(slot - 1):
        print(f"  Partition {slot} is empty — use 'n' to define it first.")
        return

    off     = PTABLE_OFFSET + (slot - 1) * ENTRY_SIZE
    current = mbr[off]
    new_val = 0x00 if current == 0x80 else 0x80
    mbr[off] = new_val
    dirty = True
    state = "Bootable (0x80)" if new_val == 0x80 else "Inactive (0x00)"
    print(f"  Partition {slot} is now {state}.")
    print("  Note: mbr88 uses the boot menu selection rather than this flag,")
    print("  but other MBR loaders may depend on it.")


def cmd_label():
    """l — set the boot menu label for a partition (mbr88 images only)."""
    global dirty
    if not has_mbr88_sig:
        print("  Labels are only supported on mbr88 images.")
        print("  Use -u when starting the program to upgrade this MBR to mbr88.")
        return

    slot = _ask_slot("  Partition number (1-4): ")
    if slot is None:
        return

    if _is_empty(slot - 1):
        print(f"  Partition {slot} is empty — use 'n' to define it first.")
        return

    current = read_label(slot - 1)
    print(f"  Current label: '{current}'")

    while True:
        text = input(f"  New label (up to {LABEL_MAX} chars): ").strip()
        if len(text) <= LABEL_MAX:
            break
        print(f"  Label too long — maximum {LABEL_MAX} characters.")

    if not text:
        text = ' '   # keep menu line non-bare

    write_label(slot - 1, text)
    dirty = True
    print(f"  Partition {slot} label set to '{text}'.")


def cmd_print():
    """p — print the partition table."""
    print_table()


def cmd_types():
    """t — list known partition type codes."""
    print()
    print("  Common partition type codes:")
    for code, name in PART_TYPES.items():
        print(f"    0x{code:02X}  {name}")
    print()


def cmd_write():
    """w — write changes to disk after confirmation."""
    global dirty, file_exists
    if not dirty:
        print("  No changes to write.")
        return

    print_table()
    if not ask_yn("Write changes to disk? (Y/N): "):
        print("  Write cancelled.")
        return

    # Backup written immediately before the target write, so the .bak file
    # only appears on disk when the target file is also about to change.
    if file_exists:
        bak = mbr_path + ".bak"
        shutil.copy2(mbr_path, bak)
        print(f"  Backup written to: {bak}")

    with open(mbr_path, 'wb') as f:
        f.write(mbr)

    active = sum(
        1 for i in range(4)
        if mbr[PTABLE_OFFSET + i * ENTRY_SIZE + 4] != 0
    )
    print(f"  Written to '{mbr_path}'.")
    print(f"  {active} active entries, {4 - active} empty slots.")
    dirty       = False
    file_exists = True   # file now definitely exists on disk


def cmd_help():
    """h — print the command summary."""
    print()
    print("  Commands:")
    print("    g  Set drive geometry (required before 'n')")
    print("    n  Define a new partition (or redefine an existing one)")
    print("    d  Delete a partition slot (zeros all 16 bytes + label)")
    print("    s  Set the partition type byte")
    print("    b  Toggle the bootable flag (0x00 / 0x80)")
    print("    l  Set the boot menu label  (mbr88 images only)")
    print("    p  Print the partition table")
    print("    t  List common partition type codes")
    print("    w  Write changes to disk")
    print("    h  This help text")
    print("    q  Quit (prompts if there are unsaved changes)")
    print()

# ---------------------------------------------------------------------------
# Program-level help text (shown with -h / --help)
# ---------------------------------------------------------------------------

HELP_TEXT = """\
mbr_patch.py — Interactive IBM XT-style MBR partition table editor

Usage:
  python mbr_patch.py <mbr_file>
  python mbr_patch.py -u <mbr_file>
  python mbr_patch.py -h | --help

Arguments:
  <mbr_file>    Path to a 512-byte MBR binary image to view or edit.
                A backup copy (<mbr_file>.bak) is written immediately
                before any changes are committed to disk.

Options:
  -u            Upgrade mode.  Replace the boot code with the mbr88
                boot record while preserving the existing partition
                table entries.  Enables full label editing regardless
                of what was in the file before.  If <mbr_file> does
                not exist it will be created from scratch as a blank
                mbr88 image ready for partition entry.
  -h, --help    Show this help text and exit.

Interactive commands (type 'h' at the prompt for a summary):
  g  Set drive geometry       n  New / redefine partition
  d  Delete partition         s  Set partition type
  b  Toggle bootable flag     l  Set boot menu label
  p  Print table              t  List type codes
  w  Write to disk            q  Quit

mbr88 signature:
  If the 5-byte signature 'mbr88' is present at offset 0x1B9, label
  editing is enabled.  Without it only the partition table entries are
  written; the rest of the MBR binary is left untouched.
  Use -u to upgrade any MBR to mbr88.

Examples:
  Edit an existing MBR:
    python mbr_patch.py mbr.bin

  Upgrade to mbr88 and set labels (preserves partition table):
    python mbr_patch.py -u mbr.bin

  Create a brand-new mbr88 image from scratch:
    python mbr_patch.py -u new.bin

  Read the MBR from a hard disk (ELKS / Linux):
    dd if=/dev/hda of=mbr.bin bs=512 count=1

  Read the MBR from a hard disk (FreeDOS):
    fdisk /smbr 1
    (saves to boot.mbr in the current directory; rename to mbr.bin)

  Write a patched MBR back to disk (ELKS / Linux — CAREFUL):
    dd if=mbr.bin of=/dev/hda bs=512 count=1
"""

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    global mbr, mbr_path, file_exists, has_mbr88_sig

    if len(sys.argv) == 2 and sys.argv[1] in ('-h', '--help'):
        print(HELP_TEXT, end='')
        sys.exit(0)

    upgrade_mode = False
    args = sys.argv[1:]
    if args and args[0] == '-u':
        upgrade_mode = True
        args = args[1:]

    if len(args) != 1:
        print("Usage: python mbr_patch.py [-u] <mbr_file>")
        print("       python mbr_patch.py -h | --help")
        sys.exit(1)

    mbr_path    = args[0]
    file_exists = os.path.isfile(mbr_path)

    if not upgrade_mode:
        if not file_exists:
            print(f"Error: '{mbr_path}' not found.")
            sys.exit(1)
        if os.path.getsize(mbr_path) < 512:
            print(f"Error: '{mbr_path}' is smaller than 512 bytes.")
            sys.exit(1)

    if file_exists:
        with open(mbr_path, 'rb') as f:
            mbr[:] = bytearray(f.read(512))
        if not upgrade_mode and (mbr[510] != 0x55 or mbr[511] != 0xAA):
            print("Warning: boot signature at 0x1FE is not 55 AA.")
    else:
        mbr[:] = bytearray(512)

    if upgrade_mode:
        upgrade_to_mbr88()
        has_mbr88_sig = True
        if file_exists:
            print("Upgrade mode: boot code replaced, partition table preserved.")
        else:
            print(f"Upgrade mode: new mbr88 image '{mbr_path}' (blank partition table).")
        print("  Use 'l' to set labels, 'w' to write when done.")
    else:
        has_mbr88_sig = detect_mbr88()
        if has_mbr88_sig:
            print("mbr88 boot record — label editing enabled.")
        else:
            print("Generic MBR — partition table editing only.")
            print("Use -u to upgrade to mbr88 and enable label editing.")

    print_table()

    # Command loop
    while True:
        try:
            raw = input("Command (h for help): ").strip().lower()
        except EOFError:
            raw = 'q'

        if not raw:
            continue

        cmd = raw[0]

        if cmd == 'q':
            if dirty and not ask_yn("Unsaved changes — quit without writing? (Y/N): "):
                continue
            break
        elif cmd == 'g':  cmd_geometry()
        elif cmd == 'n':  cmd_new()
        elif cmd == 'd':  cmd_delete()
        elif cmd == 's':  cmd_set_type()
        elif cmd == 'b':  cmd_bootable()
        elif cmd == 'l':  cmd_label()
        elif cmd == 'p':  cmd_print()
        elif cmd == 't':  cmd_types()
        elif cmd == 'w':  cmd_write()
        elif cmd == 'h':  cmd_help()
        else:
            print(f"  Unknown command '{cmd}'. Type 'h' for help.")
            continue

        # Reprint the table after every mutating command
        if cmd in ('g', 'n', 'd', 's', 'b', 'l', 'w'):
            print_table()


if __name__ == "__main__":
    main()
