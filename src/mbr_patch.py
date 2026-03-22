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

Edits the four 16-byte partition entries at offset 0x1BE in an MBR binary.
A backup copy (<mbr_file>.bak) is written before any changes are made.

IBM XT / CHS partition entry layout (16 bytes, all little-endian):
  Byte  0      Status          0x00 = inactive, 0x80 = bootable
  Bytes 1-3    CHS first sector  packed: H, S|((C>>8)<<6), C&0xFF
  Byte  4      Partition type  e.g. 0x01=FAT12, 0x04=FAT16, 0x06=FAT16B
  Bytes 5-7    CHS last sector   same packing as first sector CHS
  Bytes 8-11   LBA start       computed from CHS (little-endian DWORD)
  Bytes 12-15  LBA size        computed from CHS span  (little-endian DWORD)
"""

import sys
import os
import shutil
import struct

# ---------------------------------------------------------------------------
# CHS packing helpers
# IBM XT / MBR format packs CHS into 3 bytes:
#   byte 0: Head  (8 bits)
#   byte 1: bits 7-6 = bits 9-8 of Cylinder; bits 5-0 = Sector (1-based)
#   byte 2: bits 7-0 of Cylinder
# ---------------------------------------------------------------------------

def pack_chs(cylinder, head, sector):
    """Pack (C, H, S) into the 3-byte MBR CHS format."""
    if not (0 <= cylinder <= 1023):
        raise ValueError(f"Cylinder {cylinder} out of range 0-1023")
    if not (0 <= head <= 255):
        raise ValueError(f"Head {head} out of range 0-255")
    if not (1 <= sector <= 63):
        raise ValueError(f"Sector {sector} out of range 1-63")

    byte0 = head & 0xFF
    byte1 = (sector & 0x3F) | ((cylinder >> 2) & 0xC0)   # upper 2 bits of cylinder in bits 7-6
    byte2 = cylinder & 0xFF
    return bytes([byte0, byte1, byte2])


def chs_to_lba(cylinder, head, sector, heads_per_cylinder=16, sectors_per_track=17):
    """
    Convert CHS to a flat LBA sector number.
    IBM XT default geometry: 16 heads, 17 sectors/track.
    The caller can override for non-standard drives.
    """
    return (cylinder * heads_per_cylinder + head) * sectors_per_track + (sector - 1)


def build_entry(status, part_type,
                cyl_start, head_start, sec_start,
                cyl_end,   head_end,   sec_end,
                heads_per_cylinder, sectors_per_track):
    """Build a 16-byte partition table entry."""
    chs_first = pack_chs(cyl_start, head_start, sec_start)
    chs_last  = pack_chs(cyl_end,   head_end,   sec_end)

    lba_start = chs_to_lba(cyl_start, head_start, sec_start,
                            heads_per_cylinder, sectors_per_track)
    lba_end   = chs_to_lba(cyl_end,   head_end,   sec_end,
                            heads_per_cylinder, sectors_per_track)
    lba_size  = lba_end - lba_start + 1

    entry  = bytes([status])        # byte 0: status
    entry += chs_first              # bytes 1-3: CHS start
    entry += bytes([part_type])     # byte 4: partition type
    entry += chs_last               # bytes 5-7: CHS end
    entry += struct.pack('<I', lba_start)   # bytes 8-11:  LBA start (little-endian)
    entry += struct.pack('<I', lba_size)    # bytes 12-15: LBA size  (little-endian)
    return entry


# ---------------------------------------------------------------------------
# Common IBM XT partition type codes
# ---------------------------------------------------------------------------
PART_TYPES = {
    0x00: "Empty",
    0x01: "FAT12",
    0x04: "FAT16 (<32 MB)",
    0x05: "Extended",
    0x06: "FAT16B (>=32 MB)",
    0x0B: "FAT32",
    0x0C: "FAT32 (LBA)",
    0x80: "MINIX (old)",
    0x81: "MINIX",
    0x82: "Linux swap",
    0x83: "Linux",
}

def type_hint():
    """Return a short display string of common type codes."""
    lines = []
    for code, name in PART_TYPES.items():
        lines.append(f"  0x{code:02X} = {name}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Partition table display — decode and print existing entries
# ---------------------------------------------------------------------------

def unpack_chs(raw):
    """
    Unpack a 3-byte MBR CHS field back into (cylinder, head, sector).
    Byte layout: [head, sec|cyl_hi, cyl_lo]
      head    = byte 0
      sector  = byte 1 bits 5-0
      cylinder= (byte 1 bits 7-6) << 8 | byte 2
    """
    head     = raw[0]
    sector   = raw[1] & 0x3F
    cylinder = ((raw[1] & 0xC0) << 2) | raw[2]
    return cylinder, head, sector


def decode_entry(slot, raw16, mbr, has_mbr88_sig=False):
    """
    Decode and print one 16-byte partition table entry.
    slot      = 1-based partition number (1-4)
    raw16     = bytes or bytearray of exactly 16 bytes (the partition entry)
    mbr       = full MBR bytearray (needed to read the label slot)
    has_mbr88_sig = True if the caller detected the mbr88 signature;
                    also display the boot menu label from part_labels
    """
    status    = raw16[0]
    chs_start = raw16[1:4]
    part_type = raw16[4]
    chs_end   = raw16[5:8]
    lba_start = struct.unpack_from('<I', raw16, 8)[0]
    lba_size  = struct.unpack_from('<I', raw16, 12)[0]

    # An entry is empty if type byte and all CHS bytes are zero
    if part_type == 0x00 and all(b == 0 for b in chs_start) and all(b == 0 for b in chs_end):
        print(f"  Partition {slot}: <empty>")
        return

    cyl_s, head_s, sec_s = unpack_chs(chs_start)
    cyl_e, head_e, sec_e = unpack_chs(chs_end)

    bootable  = "Bootable" if status == 0x80 else "Inactive"
    type_name = PART_TYPES.get(part_type, "Unknown")

    # Sector size on XT-class hardware is always 512 bytes.
    # Express the partition size in the most readable unit.
    total_bytes = lba_size * 512
    if total_bytes >= 1024 * 1024:
        size_str = f"{total_bytes / (1024 * 1024):.1f} MB  ({lba_size} sectors)"
    elif total_bytes >= 1024:
        size_str = f"{total_bytes / 1024:.1f} KB  ({lba_size} sectors)"
    else:
        size_str = f"{total_bytes} bytes  ({lba_size} sectors)"

    print(f"  Partition {slot}:")
    print(f"    Status:    {bootable} (0x{status:02X})")
    print(f"    Type:      0x{part_type:02X}  ({type_name})")
    if has_mbr88_sig:
        label = decode_label_slot(mbr, slot - 1)
        print(f"    Label:     '{label}'")
    print(f"    Start CHS: C={cyl_s}, H={head_s}, S={sec_s}")
    print(f"    End   CHS: C={cyl_e}, H={head_e}, S={sec_e}")
    print(f"    LBA start: {lba_start}")
    print(f"    Size:      {size_str}")


def print_current_table(mbr, has_mbr88_sig=False):
    """Read and display all four partition entries from an MBR bytearray."""
    PTABLE_OFFSET = 0x1BE
    print("\n=== Current Partition Table ===")
    for slot in range(1, 5):
        offset = PTABLE_OFFSET + (slot - 1) * 16
        decode_entry(slot, mbr[offset:offset + 16], mbr, has_mbr88_sig)
    print()


# ---------------------------------------------------------------------------
# mbr88 signature detection
#
# mbr_nasm.asm / mbr_gas.s place the 5-byte ASCII string "mbr88" at MBR
# binary offset 0x1B9 (immediately after the last byte of executable code,
# in the reserved area before the partition table at 0x1BE).
#
# When this signature is found the MBR binary supports boot menu labels
# stored in the part_labels area at offset LABEL_BASE_OFFSET.  When it is
# absent we edit only the partition table entries and leave the rest of the
# MBR code region untouched, making the tool safe for use on any MBR image.
# ---------------------------------------------------------------------------

MBR88_SIGNATURE        = b'mbr88'
MBR88_SIGNATURE_OFFSET = 0x1B9     # offset within the 512-byte MBR binary


def detect_mbr88(mbr):
    """
    Return True if the MBR binary contains the 'mbr88' signature at 0x1B9.
    The signature is 5 bytes of ASCII with no null terminator.
    """
    return mbr[MBR88_SIGNATURE_OFFSET:
               MBR88_SIGNATURE_OFFSET + len(MBR88_SIGNATURE)] == MBR88_SIGNATURE



LABEL_BASE_OFFSET = 0x43    # offset of part_labels within the 512-byte MBR
LABEL_SLOT_SIZE   = 16      # bytes per slot (11 label + 1 null + 4 pad)
LABEL_MAX_LEN     = 11      # maximum label text length


def build_label_slot(text):
    """
    Pack a label string into a 16-byte slot.
    Layout: label text (up to 11 chars) + CR + LF + null + zero padding.

    The CR+LF must be inside the slot because print_str in the MBR
    prints bytes until it hits the null and does not add a line break
    itself.  Without CR+LF all partition labels would run together on
    one screen line.  The floppy lines are pre-built strings in the MBR
    binary that already include CR+LF, which is why those worked.

    Slot capacity: 16 bytes total.
      11 label + 2 CRLF + 1 null = 14 bytes used, 2 bytes spare padding.

    If text is empty, the slot contains just CR+LF+null+zeros so the
    menu still advances to the next line cleanly.
    """
    encoded = text[:LABEL_MAX_LEN].encode('ascii', errors='replace')
    slot    = encoded + b'\r\n\x00'                      # label + CRLF + null
    slot    = slot + b'\x00' * (LABEL_SLOT_SIZE - len(slot))  # zero-pad to 16
    return slot[:LABEL_SLOT_SIZE]


def decode_label_slot(mbr, partition_index):
    """
    Read the label string for partition_index (0-based) from the MBR bytearray.
    Returns the label as a string, stripped of trailing nulls and spaces.
    Returns an empty string if the slot is all zeros.
    """
    offset = LABEL_BASE_OFFSET + partition_index * LABEL_SLOT_SIZE
    raw    = mbr[offset:offset + LABEL_MAX_LEN + 1]
    # Find the null terminator
    null_pos = raw.find(0)
    text = raw[:null_pos] if null_pos >= 0 else raw[:LABEL_MAX_LEN]
    return text.decode('ascii', errors='replace').rstrip()




def ask_int(prompt, lo, hi):
    """Repeatedly prompt until the user enters an integer in [lo, hi]."""
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
            val = int(raw, 0)   # 0 base: accepts 0x.. or plain decimal
        except ValueError:
            print(f"  Please enter a hex (0x..) or decimal value between {lo} and {hi}.")
            continue
        if lo <= val <= hi:
            return val
        print(f"  Value must be between 0x{lo:02X} and 0x{hi:02X}.")


def ask_yn(prompt):
    """Prompt for Y/y or N/n, return True for yes."""
    while True:
        ch = input(prompt).strip()
        if ch in ('y', 'Y'):
            return True
        if ch in ('n', 'N'):
            return False
        print("  Please press Y or N.")


# ---------------------------------------------------------------------------
# Main interactive entry-collection loop
# ---------------------------------------------------------------------------

def collect_entry(entry_num, heads_per_cylinder, sectors_per_track, has_mbr88_sig=False):
    """
    Interactively collect one partition entry.
    If has_mbr88_sig is True, also collect the boot menu label for this slot.
    """
    print(f"\n--- Partition {entry_num} ---")

    # Bootable flag
    bootable = ask_yn("  Bootable? (Y/N): ")
    status   = 0x80 if bootable else 0x00

    # Partition type
    print("  Common partition types:")
    print(type_hint())
    part_type = ask_hex("  Partition type (hex or decimal, e.g. 0x06 or 6): ", 0x00, 0xFF)

    # Boot menu label — only asked for mbr88 images
    if has_mbr88_sig:
        while True:
            raw_label = input(f"  Boot menu label (up to {LABEL_MAX_LEN} chars): ").strip()
            if len(raw_label) <= LABEL_MAX_LEN:
                break
            print(f"  Label too long — maximum {LABEL_MAX_LEN} characters.")
        # Ensure at least one space so the menu line isn't bare
        label_text = raw_label if raw_label else ' '
    else:
        label_text = None

    # Starting CHS
    print("  -- Starting CHS --")
    cyl_start  = ask_int("    Cylinder (0-1023): ", 0, 1023)
    head_start = ask_int("    Head     (0-255):  ", 0, 255)
    sec_start  = ask_int("    Sector   (1-63):   ", 1, 63)

    # Ending CHS
    print("  -- Ending CHS --")
    cyl_end  = ask_int("    Cylinder (0-1023): ", 0, 1023)
    head_end = ask_int("    Head     (0-255):  ", 0, 255)
    sec_end  = ask_int("    Sector   (1-63):   ", 1, 63)

    # Build and echo summary
    lba_start = chs_to_lba(cyl_start, head_start, sec_start,
                            heads_per_cylinder, sectors_per_track)
    lba_end   = chs_to_lba(cyl_end,   head_end,   sec_end,
                            heads_per_cylinder, sectors_per_track)
    lba_size  = lba_end - lba_start + 1

    print(f"\n  Summary:")
    print(f"    Status:     {'Bootable (0x80)' if bootable else 'Inactive (0x00)'}")
    print(f"    Type:       0x{part_type:02X}  ({PART_TYPES.get(part_type, 'Unknown')})")
    if has_mbr88_sig:
        print(f"    Label:      '{label_text}'")
    print(f"    Start CHS:  C={cyl_start}, H={head_start}, S={sec_start}  (LBA {lba_start})")
    print(f"    End   CHS:  C={cyl_end},   H={head_end},   S={sec_end}    (LBA {lba_end})")
    print(f"    Size:       {lba_size} sectors")

    partition_entry = build_entry(status, part_type,
                                  cyl_start, head_start, sec_start,
                                  cyl_end,   head_end,   sec_end,
                                  heads_per_cylinder, sectors_per_track)
    label_slot = build_label_slot(label_text) if has_mbr88_sig else None
    return partition_entry, label_slot


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

HELP_TEXT = """\
mbr_patch.py — Interactive IBM XT-style MBR partition table editor

Usage:
  python mbr_patch.py <mbr_file>
  python mbr_patch.py -h | --help

Arguments:
  <mbr_file>    Path to a 512-byte MBR binary image to view or edit.
                A backup copy (<mbr_file>.bak) is written before any
                changes are made to the file.

Options:
  -h, --help    Show this help text and exit.

Description:
  Displays the current partition table stored in the MBR binary, then
  offers an interactive edit session.  The user selects which of the
  four partition slots to edit (1-4) or presses Q to quit.  Multiple
  slots can be edited in one session before writing to disk.  The file
  is only written if at least one slot was changed.

  mbr88 signature detection:
    On startup the program checks for the 5-byte signature "mbr88" at
    MBR offset 0x1B9.  This signature is written by mbr_nasm.asm and
    mbr_gas.s to identify boot records that support the extended boot
    menu label feature.

    If the signature IS found:
      - The program announces "mbr88 boot record detected".
      - Boot menu label editing is enabled for each partition slot.
      - Labels are stored in the MBR binary at offset 0x43 and are
        displayed by the bootloader in the interactive boot menu.

    If the signature is NOT found:
      - The program announces that label editing is disabled.
      - Only the partition table entries (offset 0x1BE) are written.
      - The rest of the MBR binary is left completely untouched,
        making the tool safe to use on any MBR image.

  For each partition entry the program collects:
    - Bootable flag (Y/N)
    - Partition type (hex or decimal, e.g. 0x06 or 6)
    - Boot menu label (up to 11 chars, mbr88 images only)
    - Starting CHS: cylinder, head, sector
    - Ending   CHS: cylinder, head, sector

  CHS values should be taken from fdisk or similar for the target drive.
  LBA start and size fields are computed from the CHS values using the
  drive geometry (IBM XT default: 16 heads, 17 sectors/track).

Partition type codes (common values):
  0x00  Empty
  0x01  FAT12
  0x04  FAT16 (<32 MB)
  0x05  Extended
  0x06  FAT16B (>=32 MB)
  0x0B  FAT32
  0x0C  FAT32 (LBA)
  0x80  MINIX (old)
  0x81  MINIX
  0x82  Linux swap
  0x83  Linux

  For unknown types the MBR boot menu displays '?' as the label type.

Examples:
  Edit partitions in an existing MBR binary:
    python mbr_patch.py mbr.bin

  Read the MBR from a hard disk into a file (ELKS / Linux):
    dd if=/dev/hda of=mbr.bin bs=512 count=1

  Read the MBR from a hard disk into a file (FreeDOS, native command):
    fdisk /smbr 1
    (saves the MBR of drive 1 to a file named boot.mbr in the current
     directory; rename it to mbr.bin before using with this tool)

  Write a patched MBR back to disk (ELKS / Linux — CAREFUL):
    dd if=mbr.bin of=/dev/hda bs=512 count=1

  Write a patched MBR to a floppy for testing (ELKS / Linux):
    dd if=mbr.bin of=/dev/fd0 bs=512 count=1
"""


def main():
    if len(sys.argv) == 2 and sys.argv[1] in ('-h', '--help'):
        print(HELP_TEXT, end='')
        sys.exit(0)

    if len(sys.argv) != 2:
        print("Usage: python mbr_patch.py <mbr_file>")
        print("       python mbr_patch.py -h | --help")
        sys.exit(1)

    mbr_path = sys.argv[1]

    # Verify the file exists and is at least 512 bytes
    if not os.path.isfile(mbr_path):
        print(f"Error: '{mbr_path}' not found.")
        sys.exit(1)

    if os.path.getsize(mbr_path) < 512:
        print(f"Error: '{mbr_path}' is smaller than 512 bytes — not a valid MBR image.")
        sys.exit(1)

    # Read the full MBR image into a mutable bytearray
    with open(mbr_path, 'rb') as f:
        mbr = bytearray(f.read())

    # Verify boot signature (warn but don't abort — user may be building from scratch)
    if mbr[510] != 0x55 or mbr[511] != 0xAA:
        print("Warning: boot signature at offset 0x1FE is not 55 AA — "
              "this may not be a valid MBR.")

    # Detect the mbr88 signature and tell the user what mode we're in
    has_mbr88_sig = detect_mbr88(mbr)
    if has_mbr88_sig:
        print("mbr88 boot record detected — boot menu label editing enabled.")
    else:
        print("Note: 'mbr88' signature not found at offset 0x1B9.")
        print("      Partition table editing only — label area will not be touched.")

    # Display the existing partition table before doing anything else
    print_current_table(mbr, has_mbr88_sig)

    # Give the user a chance to bail out before any changes are made
    if not ask_yn("Edit partition table? (Y/N): "):
        print("No changes made.  Exiting.")
        sys.exit(0)

    # Create backup before touching anything
    bak_path = mbr_path + ".bak"
    shutil.copy2(mbr_path, bak_path)
    print(f"\nBackup written to: {bak_path}")

    # Ask for drive geometry once — used for all entries edited this session
    print("\n=== IBM XT Drive Geometry ===")
    print("  Default IBM XT: 16 heads per cylinder, 17 sectors per track.")
    use_default = ask_yn("  Use default XT geometry (16H / 17S)? (Y/N): ")
    if use_default:
        heads_per_cylinder = 16
        sectors_per_track  = 17
    else:
        heads_per_cylinder = ask_int("  Heads per cylinder (1-255): ", 1, 255)
        sectors_per_track  = ask_int("  Sectors per track  (1-63):  ", 1, 63)

    print(f"\n  Geometry: {heads_per_cylinder} heads, {sectors_per_track} sectors/track")

    # Per-slot edit loop — user picks which slot to edit, or Q to quit
    PTABLE_OFFSET = 0x1BE
    dirty = False

    while True:
        print()
        print_current_table(mbr, has_mbr88_sig)
        print("  Enter partition number to edit (1-4) or Q to quit: ", end='', flush=True)
        choice = input().strip()

        if choice.lower() == 'q':
            break

        if choice not in ('1', '2', '3', '4'):
            print("  Please enter 1, 2, 3, 4, or Q.")
            continue

        slot_num = int(choice)   # 1-based

        # Show the current state of just this slot before editing
        slot_offset = PTABLE_OFFSET + (slot_num - 1) * 16
        print()
        decode_entry(slot_num, mbr[slot_offset:slot_offset + 16], mbr, has_mbr88_sig)

        partition_entry, label_slot = collect_entry(slot_num,
                                                    heads_per_cylinder,
                                                    sectors_per_track,
                                                    has_mbr88_sig)

        # Write the new partition entry into the in-memory MBR image
        mbr[slot_offset:slot_offset + 16] = partition_entry

        # Write the label slot only for mbr88 images — leave other MBRs untouched
        if has_mbr88_sig and label_slot is not None:
            label_offset = LABEL_BASE_OFFSET + (slot_num - 1) * LABEL_SLOT_SIZE
            mbr[label_offset:label_offset + LABEL_SLOT_SIZE] = label_slot

        dirty = True
        print(f"\n  Partition {slot_num} updated (not yet written to disk).")

    # Write to disk only if something actually changed
    if dirty:
        with open(mbr_path, 'wb') as f:
            f.write(mbr)
        active = sum(
            1 for i in range(4)
            if mbr[PTABLE_OFFSET + i * 16 + 4] != 0
        )
        print(f"\nPartition table written to '{mbr_path}'.")
        print(f"  {active} active entries, {4 - active} empty slots.")
    else:
        print("\nNo changes made.")


if __name__ == "__main__":
    main()
