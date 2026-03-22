; =============================================================================
; mbr88_nasm.asm — Custom x86 Master Boot Record
; Target CPU: Intel 8088 / 8086 (IBM PC/XT compatible)
; NASM flat-binary syntax
;
; Purpose:
;   Created to allow dual booting FreeDOS and ELKS Linux on IBM XT class
;   hardware.  Supports up to 4 hard disk partitions plus floppy drives A
;   and B.  Each partition slot carries an 11-character boot menu label.
;
; Authors:
;   Written by Claude (Anthropic) and verified by Chris Piker.
;
; License:
;   MIT License
;   Copyright (c) 2025 Chris Piker
;
;   Permission is hereby granted, free of charge, to any person obtaining
;   a copy of this software and associated documentation files (the
;   "Software"), to deal in the Software without restriction, including
;   without limitation the rights to use, copy, modify, merge, publish,
;   distribute, sublicense, and/or sell copies of the Software, and to
;   permit persons to whom the Software is furnished to do so, subject to
;   the following conditions:
;
;   The above copyright notice and this permission notice shall be
;   included in all copies or substantial portions of the Software.
;
;   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
;   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
;   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
;   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
;   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
;   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
;   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
;   SOFTWARE.
;
; AI disclosure:
;   This code was generated with Anthropic Claude, which is an AI.  It is
;   quite understandable that AI generated code may not be suitable for
;   some projects.  If you prefer not to depend on AI generated (though
;   human verified) code in your open source project, please do not
;   include this file.
;
; Acknowledgements:
;   Thanks to osdev.org for the reference material and community
;   insights that informed the design of this boot record.
;   https://wiki.osdev.org/MBR_(x86)
;
; Tested hardware platforms:
;   Author           Date        Platform
;   -------          ----------  -----------------------------------------------
;   C. Piker         2025-03-22  Leading Edge Model D, Two Floppies,
;                                SD card hard-drive with XTIDE Bios
;
; Assemble:       nasm -f bin mbr88_nasm.asm -o mbr.bin
; Verify size:    ls -l mbr.bin          (must be exactly 512 bytes)
; Verify sig:     xxd mbr.bin | tail -1  (must end in 55 aa)
; Write to disk:  dd if=mbr.bin of=/dev/sdX bs=512 count=1  *** CAREFUL ***
;
; 8088 instruction set constraints observed throughout:
;   - No 32-bit registers (EAX/EBX/etc are 386+)
;   - No PUSH immediate (added on 80186; use MOV reg,imm / PUSH reg instead)
;   - Shift/rotate only by 1 or by CL (immediate count added on 80186)
;   - No INT 13h/42h Extended Read (late-1990s BIOS extension, not on XT)
;   - All disk I/O uses INT 13h/02h CHS read — the only read call on XT BIOS
;
; Disk reads use the CHS values stored in the partition table entry itself:
;   Entry +1  Head of first sector
;   Entry +2  Sector and upper cylinder bits (packed: bits 7-6 = C[9:8], bits 5-0 = S)
;   Entry +3  Lower 8 bits of cylinder
;
; Memory map:
;   0x0000:0x7C00  — BIOS loads us here initially
;   0x0000:0x7A00  — we relocate here, freeing 0x7C00 for the VBR
;
; Boot menu format:
;   Boot:
;   A Floppy
;   B Floppy
;   1 <label>       (only shown if partition slot is non-empty)
;   >
;
; Labels are stored in part_labels (four 16-byte slots).
; Each slot: 11 chars + CR + LF + null + 2 pad bytes.
; mbr_patch.py / mbr_patch.c write them alongside the partition table entries.
;
; Valid keystrokes:
;   A / a  — boot floppy drive A  (BIOS drive 00h)
;   B / b  — boot floppy drive B  (BIOS drive 01h)
;   1 - 4  — boot that hard disk partition (ignored if slot is empty)
;
; VBR handoff:
;   A CR+LF is emitted before the far jump so VBR output starts on a
;   fresh line.  DL = boot drive number (IBM BIOS convention).
;
; CX/CL NOTE — critical aliasing issue in display_list:
;   LOOP uses CX as its 16-bit counter.  CL is the low byte of CX.
;   MOV CL,4 inside the loop body would reset CX to 4 every non-empty
;   iteration, preventing LOOP from counting down to zero and causing
;   the loop to run far past 4 entries.  We save/restore CX on the
;   stack (PUSH CX / POP CX) around the SHL sequences to avoid this.
;
; Relocation delta:
;   ORG is 0x7C00 so every label assembles to a 0x7Cxx address.
;   After relocation the bytes live at 0x7A00+offset.
;   RDELTA (= -0x0200) is added to every label referenced after the
;   far-jump so the reference lands in the relocated copy.
; =============================================================================

        bits    16
        org     7C00h

RDELTA  equ     7A00h - 7C00h           ; = -0x0200

; =============================================================================
; String and data table
; =============================================================================

        jmp     near start              ; 3-byte near jump over data area

str_boot_from:
        db      'Boot:', 0Dh, 0Ah, 0    ; "Boot:\r\n\0"  8 bytes

str_floppy_ab:
        db      'A Floppy', 0Dh, 0Ah   ; "A Floppy\r\n"
        db      'B Floppy', 0Dh, 0Ah, 0 ; "B Floppy\r\n\0"  22 bytes total

str_disk_err:
        db      'Read error', 0Dh, 0Ah, 0  ; 13 bytes
        ; Note: the \r\n\0 at offset +10 is reused as the pre-VBR newline.
        ; See do_chs_read — SI is pointed at str_disk_err+10+RDELTA to
        ; emit just "\r\n\0" before the far jump to the VBR.

str_no_boot:
        db      ': No boot record', 0Dh, 0Ah, 0  ; 19 bytes

scratch_char:
        db      0                       ; display char of last selected device

; part_labels — four 16-byte slots, one per partition entry.
; Slot layout:
;   Bytes  0-10  Label text (up to 11 ASCII characters)
;   Bytes 11-12  CR + LF (0x0D 0x0A) — required for line break
;   Byte   13    Null terminator (stops print_str)
;   Bytes 14-15  Zero padding
; mbr_patch.py writes all four slots when the user edits partition entries.
part_labels:
        times   64  db 0                ; 4 slots x 16 bytes

; =============================================================================
; Entry point
; =============================================================================
start:
        cli
        xor     ax, ax
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, 7A00h
        sti

; =============================================================================
; RELOCATION — copy 512 bytes from 0x7C00 to 0x7A00
; =============================================================================
relocate:
        mov     si, 7C00h
        mov     di, 7A00h
        mov     cx, 256
        rep     movsw

        db      0EAh
        dw      7A00h + (after_reloc - 7C00h)
        dw      0000h

; =============================================================================
; Execution continues here inside the RELOCATED copy at 0x7A00.
; =============================================================================
after_reloc:

; Print header, then the menu list, then the prompt
        mov     si, str_boot_from + RDELTA  ; "Boot:\r\n"
        call    print_str

        call    display_list            ; floppy lines + partition lines

        ; Print the ">" prompt.
        ; AH=0Eh and BH=00h are still set from the last print_str call
        ; inside display_list, so we only need to load AL and call INT 10h.
        mov     al, '>'
        int     10h

; =============================================================================
; Wait for a valid keypress
; =============================================================================
wait_key:
        mov     ah, 00h
        int     16h                     ; AL = ASCII

        cmp     al, 'a'
        je      boot_floppy_a
        cmp     al, 'A'
        je      boot_floppy_a

        cmp     al, 'b'
        je      boot_floppy_b
        cmp     al, 'B'
        je      boot_floppy_b

        cmp     al, '1'
        jb      wait_key
        cmp     al, '4'
        ja      wait_key

        sub     al, '1'                 ; '1'->0  '2'->1  '3'->2  '4'->3
        mov     bl, al

        xor     ah, ah
        mov     cl, 4
        shl     ax, cl                  ; AX = index * 16
        add     ax, 7BBEh
        mov     si, ax

        cmp     byte [si+4], 00h        ; reject empty slot
        je      wait_key

        ; Echo chosen digit and save for error reporting
        mov     ah, 0Eh
        mov     al, bl
        add     al, '1'
        mov     bh, 00h
        int     10h
        mov     [scratch_char + RDELTA], al

        mov     dl, 80h
        mov     dh, [si+1]
        mov     cl, [si+2]
        mov     ch, [si+3]
        jmp     do_chs_read

; =============================================================================
; Floppy boot paths
; =============================================================================
boot_floppy_a:
        mov     dl, 00h
        jmp     do_floppy

boot_floppy_b:
        mov     dl, 01h

do_floppy:
        mov     ah, 0Eh
        mov     al, 'A'
        cmp     dl, 01h
        jne     .echo
        mov     al, 'B'
.echo:
        mov     bh, 00h
        int     10h
        mov     [scratch_char + RDELTA], al

        mov     ch, 00h
        mov     cl, 01h
        mov     dh, 00h

; =============================================================================
; do_chs_read — read one sector into 0x7C00, verify VBR sig, jump.
;
; DL is saved in BL across INT 13h/00h reset (some XT BIOSes corrupt DL).
; On success a CR+LF is emitted before the far jump so the VBR's first
; output lands on a fresh line.  This is done by pointing print_str at
; the \r\n\0 that is already embedded inside str_disk_err at offset +10,
; saving us the 8 bytes an inline MOV/INT sequence would cost.
; =============================================================================
do_chs_read:
        mov     bl, dl                  ; save drive number

        mov     ah, 00h                 ; INT 13h/00h = reset
        int     13h
        jc      disk_error

        mov     dl, bl                  ; restore drive number

        mov     ah, 02h                 ; INT 13h/02h = read sectors
        mov     al, 01h
        mov     bx, 7C00h
        int     13h
        jc      disk_error

        ; VBR signature check
        cmp     byte [7DFEh], 55h
        jne     bad_vbr
        cmp     byte [7DFFh], 0AAh
        jne     bad_vbr

        ; Emit CR+LF before handing control to the VBR so its first output
        ; starts on a fresh line.  Point SI at the \r\n\0 embedded inside
        ; str_disk_err at offset +10 (after "Read error") — no new string needed.
        mov     si, str_disk_err + 10 + RDELTA
        call    print_str               ; prints "\r\n" then stops at null

        ; Far-jump to VBR — DL = boot drive (IBM BIOS convention)
        db      0EAh
        dw      7C00h
        dw      0000h

disk_error:
        mov     si, str_disk_err + RDELTA
        call    print_str
        jmp     after_reloc

bad_vbr:
        mov     ah, 0Eh
        mov     al, [scratch_char + RDELTA]
        mov     bh, 00h
        int     10h
        mov     si, str_no_boot + RDELTA
        call    print_str
        jmp     after_reloc

; =============================================================================
; display_list — print the multi-line boot option menu.
;
; Prints:
;   A Floppy\r\n
;   B Floppy\r\n
;   N <label>\r\n   (for each non-empty partition, N = '1'..'4')
;
; CX/CL aliasing: LOOP decrements the full 16-bit CX.  The shift count
; for index*16 is loaded into CL (= low byte of CX) which would reset
; CX and break the loop counter.  We save CX on the stack before any
; MOV CL,4 and restore it after all SHL operations in that path.
; =============================================================================
display_list:
        ; Floppy lines (both in one pre-built string, one call)
        mov     si, str_floppy_ab + RDELTA  ; "A Floppy\r\nB Floppy\r\n\0"
        call    print_str

        ; Partition lines
        mov     si, 7BBEh               ; SI -> first partition entry
        mov     bl, '1'                 ; starting digit
        mov     cx, 4                   ; exactly 4 entries

.part_loop:
        cmp     byte [si+4], 00h        ; type byte 00h = empty slot
        je      .next

        ; Print digit
        mov     ah, 0Eh
        mov     bh, 00h
        mov     al, bl
        int     10h

        ; Print space
        mov     al, ' '
        int     10h

        ; Compute label address = part_labels + RDELTA + index*16
        ; PUSH CX before touching CL to protect the LOOP counter.
        push    cx                      ; save LOOP counter — MOV CL,4 clobbers CX
        mov     al, bl
        sub     al, '1'                 ; 0-based index
        xor     ah, ah                  ; AX = index
        mov     cl, 4
        shl     ax, cl                  ; AX = index * 16
        add     ax, part_labels + RDELTA
        mov     si, ax
        call    print_str               ; print label (contains \r\n\0)

        ; Recalculate SI to current partition entry (CL still 4, CX still pushed)
        mov     al, bl
        sub     al, '1'
        xor     ah, ah
        shl     ax, cl                  ; AX = index * 16  (CL still = 4)
        add     ax, 7BBEh
        mov     si, ax
        pop     cx                      ; restore LOOP counter

.next:
        inc     bl
        add     si, 16
        loop    .part_loop

        ret

; =============================================================================
; print_str — print null-terminated string via BIOS INT 10h/0Eh.
; On entry: SI -> string, DS = 0
; Trashes:  AX, BH
; =============================================================================
print_str:
        mov     ah, 0Eh
        mov     bh, 00h
.loop:
        lodsb                           ; AL = [SI], SI++
        test    al, al                  ; null terminator?
        jz      .done
        int     10h
        jmp     .loop
.done:
        ret

; =============================================================================
; Signature and partition table.
;
; "mbr88" (5 bytes, no null) is placed at MBR offset 0x1B9, immediately
; after the last byte of code.  It fills the reserved area 0x1B9-0x1BD,
; leaving 0x1BD as zero and the partition table beginning at 0x1BE as
; required.  Magic numbers are identified by position and length, not by
; null termination — 0x55AA itself has no null.
;
; Offset 0x1B8 (the first reserved byte) is zero-padded by the code area.
; Offsets 0x1B8-0x1BD are the conventional disk signature reserved area.
; Windows uses 0x1B8-0x1BB as a 4-byte disk serial number; we target
; XT-class hardware only so these bytes are available for our use.
; =============================================================================
        times   1B9h - ($ - $$)  db 0  ; pad any remaining gap (currently 0 bytes)

mbr_sig:
        db      'mbr88'                 ; 5-byte magic at 0x1B9-0x1BD: 6D 62 72 38 38

part_table:
        times   64  db 0                ; written by mbr_patch.py

        times   510 - ($ - $$)  db 0
        db      55h
        db      0AAh
