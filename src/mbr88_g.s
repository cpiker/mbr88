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
 *
 * CX/CL aliasing in display_list:
 *   LOOP uses the full 16-bit CX as its counter.  CL is the low byte of CX.
 *   MOVB $4,%cl inside the loop body would silently reset CX to 4 on every
 *   non-empty iteration, preventing the loop from ever counting down to zero.
 *   We PUSHW %cx before loading %cl and POPW %cx after all SHLW operations
 *   to protect the counter across the label-address calculation.
 *
 * cbtw used for zero-extension (saves 1 byte vs xorb %ah,%ah):
 *   In two places the code needs AH=0 to zero-extend AL into AX before a
 *   multiply-by-16 shift.  AL holds a partition index (always 0-3), so it
 *   is never negative and CBTW (sign-extend AL into AX) produces the same
 *   result as XORB %AH,%AH.  CBTW encodes in 1 byte vs 2 for XORB.
 *   The byte saved in wait_key was spent on the explicit MOVB $0x0E,%AH
 *   before the '>' prompt (see below).
 *
 * AH undefined after INT 10h/0Eh:
 *   The BIOS specification leaves AH undefined on return from INT 10h/0Eh
 *   (teletype output).  Relying on AH surviving the call is fragile and
 *   breaks on some real and emulated BIOSes.  Every INT 10h call in this
 *   code is preceded by an explicit MOVB $0x0E,%AH.  The '>' prompt after
 *   display_list therefore sets AH explicitly rather than assuming it
 *   survives from the last call inside display_list.  The 2-byte cost of
 *   MOVB $0x0E,%AH was funded by the 1-byte cbtw saving plus the last
 *   zero pad byte. Subsequent changes have restored ~7 bytes of slack.
 *
 * Stack placement:
 *   SP is initialised to 0x6000 — the base address of our relocated copy.
 *   The 8088 stack grows downward, so pushes begin writing below 0x6000
 *   into the unused region 0x5FFE, 0x5FFC, ... well away from both the
 *   IVT (0x0000) and the relocated code (0x6000-0x61FF), giving roughly
 *   22 KB of safe stack space.
 * ===========================================================================*/

    .arch   i8086,jumps
    .code16

    .equ    RDELTA, 0x6000 - 0x7C00    # = -0x1C00
    # PTABLE: runtime address of the partition table in the relocated image.
    # part_table assembles at ORG-base 0x7C00 + 0x1BE = 0x7DBE (load-time).
    # After relocation to 0x6000: 0x7DBE + RDELTA = 0x7DBE - 0x1C00 = 0x61BE.
    # GAS cannot use label+RDELTA in an addw $imm operand the way NASM can,
    # so the value is pre-computed here as a named constant.
    .equ    PTABLE, 0x7C00 + 0x1BE + RDELTA  # = 0x61BE

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
    .byte   0                       # combined 21 bytes

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
    movw    $0x6000, %sp            # stack grows down from 0x6000 into free low memory
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
    # BH=0x00 is preserved across INT 10h/0Eh (page number, never modified on return).
    # AH is undefined on return from INT 10h/0Eh on some BIOSes, so set explicitly.
    # The byte cost is funded by cbtw in wait_key (below).
    movb    $0x0E, %ah
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
    movb    %al, %bl                # save index in BL — cbtw below zeros AH,
                                    # leaving BL as the only intact copy of the index

    cbtw                            # zero-extend AL into AX (AL is 0-3, never negative,
                                    # so CBTW = XORB %AH,%AH here but costs 1 byte instead of 2;
                                    # the saved byte was spent on MOVB $0x0E,%AH for the '>' prompt)
    movb    $4, %cl
    shlw    %cl, %ax                # AX = index * 16
    addw    $PTABLE, %ax
    movw    %ax, %si

    cmpb    $0x80, (%si)            # only accept bootable (0x80) partitions;
    jne     wait_key                # status 0x00 covers both empty and non-bootable slots

    # Save chosen digit char for error reporting (bad_vbr prints it).
    # No explicit echo — bad_vbr echoes scratch_char on failure,
    # and on success the pre-VBR CR+LF fires cleanly without a digit.
    # INT 16h/00h does not echo keystrokes on PC BIOS.
    movb    %bl, %al
    addb    $'1', %al               # convert 0-based index back to digit char
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
    # Set AL = 'A' or 'B' for scratch_char (bad_vbr prints it on failure).
    # No explicit echo — same reasoning as wait_key above.
    movb    $'A', %al
    cmpb    $0x01, %dl
    jne     .Lset_scratch
    movb    $'B', %al
.Lset_scratch:
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
 *
 * Register allocation inside do_chs_read:
 *   BL  drive number — saved here because INT 13h/00h reset may corrupt DL.
 *   DI  CHS fields (CX on entry) — callers load CL=sector|cyl-hi, CH=cyl-lo
 *       into CX before jumping here.  CX is immediately reused as the retry
 *       counter (MOVW $3,%cx), so CHS is saved to DI first.  Before each read
 *       call CX is temporarily swapped back to CHS (PUSHW %cx / MOVW %di,%cx),
 *       and the counter is restored from the stack after INT 13h (POPW %cx).
 *       DI is not touched by INT 13h on XT BIOS.
 * =========================================================================*/
do_chs_read:
    movb    %dl, %bl                # save drive number across INT 13h reset
    movw    %cx, %di                # save CHS — CX reused as retry counter below
    movw    $3, %cx                 # retry counter (3 attempts)

.Lretry:
    movb    $0x00, %ah              # INT 13h/00h = reset
    int     $0x13
    # jc not checked after reset — a reset failure produces a read
    # failure on the next instruction, caught by the retry loop.

    movb    %bl, %dl                # restore drive number (BIOS may corrupt)
    pushw   %cx                     # save retry counter — CX needed for CHS
    movw    %di, %cx                # restore CHS fields for INT 13h/02h

    movb    $0x02, %ah              # INT 13h/02h = read sectors
    movb    $0x01, %al
    movw    $0x7C00, %bx            # ES:BX = read buffer (ES=0 set at startup)
    int     $0x13
    popw    %cx                     # restore retry counter
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

    movw    $PTABLE, %si            # SI -> first partition entry
    movb    $'1', %bl               # starting digit
    movw    $4, %cx                 # exactly 4 entries

.Lpart_loop:
    cmpb    $0x80, (%si)            # only show bootable (0x80) partitions;
    jne     .Lnext_entry            # status 0x00 covers both empty and non-bootable slots

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
    cbtw                            # zero-extend AL into AX (AL is 0-3; CBTW = XORB %AH,%AH here,
                                    # 1 byte vs 2 — see file header note on cbw byte budget)
    movb    $4, %cl
    shlw    %cl, %ax                # AX = index * 16
    addw    $part_labels+RDELTA, %ax
    movw    %ax, %si
    call    print_str               # print label (contains \r\n\0)

    # Recalculate SI to partition entry (%cl still 4, %cx still on stack)
    movb    %bl, %al
    subb    $'1', %al
    cbtw                            # zero-extend AL into AX (CBTW = XORB %AH,%AH, 1 byte vs 2)
    shlw    %cl, %ax                # AX = index * 16  (%cl still = 4)
    addw    $PTABLE, %ax
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
 * The binary currently has approximately 7 bytes of slack before the signature.
 *
 * .org is relative to section base (0x7C00 set by -Ttext=0x7C00).
 * =========================================================================*/
    .org    0x1B8               # advance to signature (~7 bytes slack currently)

mbr_sig:
    .ascii  "mbr88"             # 5-byte signature: 6D 62 72 38 38
mbr_ver:
    .byte   0x01                # version 0.1 (major=0, minor=1)

part_table:
    .fill   64, 1, 0            # written by mbr_patch

    .org    0x1FE
    .byte   0x55
    .byte   0xAA
