/* ===========================================================================
 * mbr88_gas.s — Custom x86 Master Boot Record
 * Target CPU: Intel 8088 / 8086 (IBM PC/XT compatible)
 * GNU Assembler (GAS) AT&T syntax
 *
 * Purpose:
 *   Created to allow dual booting FreeDOS and ELKS Linux on IBM XT class
 *   hardware.  Supports up to 4 hard disk partitions plus floppy drives A
 *   and B.  Each partition slot carries an 11-character boot menu label.
 *
 * Authors:
 *   Written by Claude (Anthropic) and verified by Chris Piker.
 *
 * License:
 *   MIT License
 *   Copyright (c) 2025 Chris Piker
 *
 *   Permission is hereby granted, free of charge, to any person obtaining
 *   a copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be
 *   included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 *   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 *   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 *   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 * AI disclosure:
 *   This code was generated with Anthropic Claude, which is an AI.  It is
 *   quite understandable that AI generated code may not be suitable for
 *   some projects.  If you prefer not to depend on AI generated (though
 *   human verified) code in your open source project, please do not
 *   include this file.
 *
 * Acknowledgements:
 *   Thanks to osdev.org for the reference material and community
 *   insights that informed the design of this boot record.
 *   https://wiki.osdev.org/MBR_(x86)
 *
 * Tested hardware platforms:
 *   Author           Date        Platform
 *   -------          ----------  -----------------------------------------------
 *   C. Piker         2025-03-22  Leading Edge Model D, Two Floppies,
 *                                SD card hard-drive with XTIDE Bios
 *
 * Assemble and link to a flat binary:
 *   ia16-elf-as -o mbr88_gas.o mbr88_gas.s
 *   ia16-elf-ld -Ttext=0x7C00 --oformat=binary -o mbr.bin mbr88_gas.o
 *
 * Verify:
 *   ls -l mbr.bin          (must be exactly 512 bytes)
 *   xxd mbr.bin | tail -1  (must end in 55 aa)
 *
 * See mbr88_nasm.asm for full design notes.  This file is a faithful AT&T
 * syntax translation — binary output is identical to the NASM version.
 *
 * AT&T syntax reminders:
 *   Operand order reversed: src, dest
 *   Registers: %ax %si %al etc.
 *   Immediates: $0x10 $'A' $4 etc.
 *   Memory: disp(%reg) for [reg+disp],  0x7DFE for absolute [7DFEh]
 *   Size suffixes: b=byte  w=word
 *   Local labels: .Lname
 *   Comments: # to end of line, or C-style block comments
 *   .org N is relative to section base (set by -Ttext=0x7C00 at link time)
 *   DO NOT use ". = 0x7C00 + N" — that measures from absolute zero
 *   and produces ~31 KB of unwanted padding in the output binary.
 *
 * Jump size optimization warning:
 *   GAS will optimize 'jmp label' to a 2-byte short jump (EB) whenever
 *   the displacement fits in a signed byte (±127).  NASM's 'jmp near'
 *   always emits a 3-byte near jump (E9).  To force a near jump in GAS
 *   and maintain binary identity with the NASM version, the initial jump
 *   is emitted as raw bytes:
 *     .byte 0xE9
 *     .word start - (_start + 3)
 *   This is exactly what the _start jump at the top of this file does.
 *
 * Far jumps:
 *   Two far jumps appear in this code, both encoded as raw bytes rather
 *   than using the GAS 'ljmp' mnemonic.  This is necessary because GAS
 *   far jump syntax does not accept arithmetic expressions in the offset
 *   field — only simple labels or literal values.  The raw encoding
 *   (0xEA followed by offset word and segment word) is the standard
 *   workaround used in MBR and bootloader code.
 * ===========================================================================*/

    .arch   i8086,jumps
    .code16

    .equ    RDELTA, 0x6000 - 0x7C00    # = -0x1C00

/* ---------------------------------------------------------------------------
 * String and data table
 * --------------------------------------------------------------------------*/

    .global _start
_start:
    # Force a 3-byte near jump (opcode E9) over the data area.
    # GAS would optimize 'jmp start' to a 2-byte short jump (EB) because
    # the target is exactly 127 bytes away — the last value that fits in a
    # signed byte.  That shifts every subsequent byte vs the NASM binary.
    # We emit the opcode and displacement explicitly, matching NASM's
    # 'jmp near start' which always produces E9 xx xx.
    .byte   0xE9
    .word   start - (_start + 3)    # displacement from end of this instruction

str_boot_from:
    .ascii  "Boot:\r\n"
    .byte   0                       # "Boot:\r\n\0"  8 bytes

str_floppy_ab:
    .ascii  "A Floppy\r\n"
    .ascii  "B Floppy\r\n"
    .byte   0                       # combined 22 bytes

str_disk_err:
    .ascii  "Read error\r\n"
    .byte   0                       # 13 bytes
    # The \r\n\0 at offset +10 is reused as the pre-VBR newline emitter.
    # str_disk_err+10+RDELTA points directly to "\r\n\0" in relocated copy.

str_no_boot:
    .ascii  ": No boot record\r\n"
    .byte   0                       # 19 bytes

scratch_char:
    .byte   0                       # display char of last selected device

# part_labels — four 16-byte slots, one per partition entry.
# Slot layout:
#   Bytes  0-10  Label text (up to 11 ASCII chars)
#   Bytes 11-12  CR + LF (0x0D 0x0A) — required for line break
#   Byte   13    Null terminator
#   Bytes 14-15  Zero padding
part_labels:
    .fill   64, 1, 0                # 4 slots x 16 bytes

/* ===========================================================================
 * Entry point
 * =========================================================================*/
start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x6000, %sp
    sti

/* ===========================================================================
 * Relocation — copy 512 bytes from 0x7C00 to 0x6000, then far jump there.
 *
 * The far jump is encoded as raw bytes (0xEA + offset word + segment word)
 * because GAS far jump syntax does not accept arithmetic expressions in
 * the offset field.  The target is after_reloc in the relocated copy:
 *   offset = 0x6000 + (after_reloc - 0x7C00)
 *   segment = 0x0000
 * =========================================================================*/
relocate:
    movw    $0x7C00, %si
    movw    $0x6000, %di
    movw    $256, %cx
    rep     movsw

    .byte   0xEA                            # far jump opcode
    .word   0x6000 + (after_reloc - 0x7C00) # offset in relocated copy
    .word   0x0000                          # segment

/* ===========================================================================
 * Execution continues here in the relocated copy at 0x6000.
 * =========================================================================*/
after_reloc:
    movw    $str_boot_from+RDELTA, %si
    call    print_str               # print "Boot:\r\n"

    call    display_list            # floppy lines + partition lines

    # Print ">" prompt.
    # AH=0Eh and BH=0x00 are still set from the last print_str call inside
    # display_list, so only AL needs loading before INT 10h.
    movb    $'>', %al
    int     $0x10

/* ===========================================================================
 * Wait for a valid keypress
 * =========================================================================*/
wait_key:
    movb    $0x00, %ah
    int     $0x16                   # AL = ASCII

    cmpb    $'a', %al
    je      boot_floppy_a
    cmpb    $'A', %al
    je      boot_floppy_a

    cmpb    $'b', %al
    je      boot_floppy_b
    cmpb    $'B', %al
    je      boot_floppy_b

    cmpb    $'1', %al
    jb      wait_key
    cmpb    $'4', %al
    ja      wait_key

    subb    $'1', %al               # '1'->0  '2'->1  '3'->2  '4'->3
    movb    %al, %bl

    xorb    %ah, %ah
    movb    $4, %cl
    shlw    %cl, %ax                # AX = index * 16
    addw    $0x7BBE, %ax
    movw    %ax, %si

    cmpb    $0x00, 4(%si)           # reject empty slot
    je      wait_key

    movb    $0x0E, %ah
    movb    %bl, %al
    addb    $'1', %al
    movb    $0x00, %bh
    int     $0x10
    movb    %al, scratch_char+RDELTA

    movb    $0x80, %dl
    movb    1(%si), %dh
    movb    2(%si), %cl
    movb    3(%si), %ch
    jmp     do_chs_read

/* ===========================================================================
 * Floppy boot paths
 * =========================================================================*/
boot_floppy_a:
    movb    $0x00, %dl
    jmp     do_floppy

boot_floppy_b:
    movb    $0x01, %dl

do_floppy:
    movb    $0x0E, %ah
    movb    $'A', %al
    cmpb    $0x01, %dl
    jne     .Lecho_letter
    movb    $'B', %al
.Lecho_letter:
    movb    $0x00, %bh
    int     $0x10
    movb    %al, scratch_char+RDELTA

    movb    $0x00, %ch
    movb    $0x01, %cl
    movb    $0x00, %dh

/* ===========================================================================
 * do_chs_read — read sector, verify VBR signature, emit CR+LF, jump.
 *
 * DL saved in BL across reset (some XT BIOSes corrupt DL during reset).
 * Pre-VBR CR+LF emitted by calling print_str on the \r\n\0 embedded inside
 * str_disk_err at offset +10, avoiding an 8-byte inline MOV/INT sequence.
 *
 * The far jump to the VBR is encoded as raw bytes (0xEA + 0x7C00 + 0x0000)
 * because GAS far jump syntax does not accept arithmetic expressions in
 * the offset field — though in this case the target is a literal 0x7C00
 * and raw encoding is used here for consistency with the relocation jump.
 *
 * Retry loop: floppy drives require up to 3 attempts to allow the disk
 * motor time to reach stable speed.  The reset is retried each pass.
 * If the reset itself fails we proceed to the read anyway — a failed reset
 * will produce a failed read, handled by the retry loop.  After 3 consecutive
 * read failures we report disk_error.  This follows the convention of most
 * XT-class MBR boot blocks.
 * =========================================================================*/
do_chs_read:
    movb    %dl, %bl                # save drive number across reset

    movw    $3, %cx                 # retry counter (3 attempts)
.Lretry:
    movb    $0x00, %ah              # INT 13h/00h = reset
    int     $0x13
    # jc not checked after reset — a reset failure produces a read
    # failure on the next instruction, caught by the retry loop.

    movb    %bl, %dl                # restore drive number (BIOS may corrupt)

    movb    $0x02, %ah              # INT 13h/02h = read sectors
    movb    $0x01, %al
    movw    $0x7C00, %bx
    int     $0x13
    jnc     .Lread_ok               # success — proceed to VBR check
    loop    .Lretry                 # read failed — decrement CX, retry
    jmp     disk_error              # all 3 attempts failed
.Lread_ok:

    cmpb    $0x55, 0x7DFE
    jne     bad_vbr
    cmpb    $0xAA, 0x7DFF
    jne     bad_vbr

    # Emit CR+LF before handing off to the VBR.
    # Point SI at offset +10 within str_disk_err which is "\r\n\0".
    movw    $str_disk_err+10+RDELTA, %si
    call    print_str

    # Far-jump to VBR at 0x0000:0x7C00 — DL = boot drive (IBM BIOS convention).
    # Encoded as raw bytes for consistency with the relocation jump above.
    .byte   0xEA
    .word   0x7C00
    .word   0x0000

disk_error:
    movw    $str_disk_err+RDELTA, %si
    call    print_str
    jmp     after_reloc

bad_vbr:
    movb    $0x0E, %ah
    movb    scratch_char+RDELTA, %al
    movb    $0x00, %bh
    int     $0x10
    movw    $str_no_boot+RDELTA, %si
    call    print_str
    jmp     after_reloc

/* ===========================================================================
 * display_list — print the multi-line boot option menu.
 *
 * CX/CL aliasing fix: LOOP uses full 16-bit CX as counter.  CL is its
 * low byte.  MOVB $4, %cl inside the loop would reset CX to 4 every
 * non-empty iteration, preventing LOOP from ever reaching zero.
 * We PUSH %cx before loading %cl and POP %cx after all SHLW operations.
 * =========================================================================*/
display_list:
    movw    $str_floppy_ab+RDELTA, %si  # "A Floppy\r\nB Floppy\r\n\0"
    call    print_str

    movw    $0x7BBE, %si            # SI -> first partition entry
    movb    $'1', %bl               # starting digit
    movw    $4, %cx                 # exactly 4 entries

.Lpart_loop:
    cmpb    $0x00, 4(%si)           # type byte 0x00 = empty
    je      .Lnext_entry

    movb    $0x0E, %ah
    movb    $0x00, %bh
    movb    %bl, %al
    int     $0x10                   # print digit

    movb    $' ', %al
    int     $0x10                   # print space

    # Compute label address = part_labels + RDELTA + index*16
    # PUSH %cx before MOVB $4, %cl to protect the LOOP counter.
    pushw   %cx                     # save LOOP counter
    movb    %bl, %al
    subb    $'1', %al               # 0-based index
    cbtw                            # AX = index (AL is 0-3; cbtw = xorb %ah,%ah here)
    movb    $4, %cl
    shlw    %cl, %ax                # AX = index * 16
    addw    $part_labels+RDELTA, %ax
    movw    %ax, %si
    call    print_str               # print label (contains \r\n\0)

    # Recalculate SI to partition entry (%cl still 4, %cx still on stack)
    movb    %bl, %al
    subb    $'1', %al
    cbtw                            # AX = index (cbtw saves 1 byte vs xorb %ah,%ah)
    shlw    %cl, %ax                # AX = index * 16  (%cl still = 4)
    addw    $0x7BBE, %ax
    movw    %ax, %si
    popw    %cx                     # restore LOOP counter

.Lnext_entry:
    incb    %bl
    addw    $16, %si
    loop    .Lpart_loop

    ret

/* ===========================================================================
 * print_str — print null-terminated string via BIOS INT 10h/0Eh.
 * On entry: SI -> string, DS = 0
 * Trashes:  AX, BH
 * =========================================================================*/
print_str:
    movb    $0x0E, %ah
    movb    $0x00, %bh
.Lps_loop:
    lodsb                           # AL = [SI], SI++
    testb   %al, %al                # null terminator?
    jz      .Lps_done
    int     $0x10
    jmp     .Lps_loop
.Lps_done:
    ret

/* ===========================================================================
 * Signature and partition table.
 *
 * 'mbr88' (5 bytes) is placed at MBR offset 0x1B8 followed immediately by
 * the version byte 0x01 at 0x1BD.  Version encoding: high nibble = major,
 * low nibble = minor (0x01 = v0.1).
 *
 * The binary is fully packed — only 1 zero pad byte remains at 0x1B7.
 *
 * .org is relative to section base (0x7C00 set by -Ttext=0x7C00).
 * =========================================================================*/
    .org    0x1B8               # advance to signature (1 zero pad byte at 0x1B7)

mbr_sig:
    .ascii  "mbr88"             # 5-byte signature: 6D 62 72 38 38
mbr_ver:
    .byte   0x01                # version 0.1 (major=0, minor=1)

part_table:
    .fill   64, 1, 0            # written by mbr_patch

    .org    0x1FE
    .byte   0x55
    .byte   0xAA
