; =============================================================================
; mbr88.asm — Custom x86 Master Boot Record
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
;   Copyright (c) 2026 Chris Piker
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
; Assemble:       nasm -f bin src/mbr88.asm -o build/native/mbr88.bin
; Verify size:    ls -l build/native/mbr88.bin   (must be exactly 512 bytes)
; Verify sig:     xxd build/native/mbr88.bin | tail -1  (must end in 55 aa)
; Write to disk:  dd if=mbr88.bin of=/dev/sdX bs=512 count=1  *** CAREFUL ***
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
;   0x0000:0x6000  — we relocate here (traditional MBR relocation address),
;                    freeing 0x7C00 for the VBR.  Gives ~22 KB of stack space.
;
; Boot menu format:
;   Boot:
;   A Floppy
;   B Floppy
;   1 <label>       (only shown if partition slot is bootable, status=0x80)
;   >
;
; Labels are stored in part_labels (four 16-byte slots).
; Each slot: 1-11 printable ASCII chars + CR + LF + null + zero padding.
; mbrpatch writes them alongside the partition table entries.
;
; Valid keystrokes:
;   A / a  — boot floppy drive A  (BIOS drive 00h)
;   B / b  — boot floppy drive B  (BIOS drive 01h)
;   1 - 4  — boot that hard disk partition (ignored if slot is not bootable)
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
; cbw used for zero-extension (saves 1 byte vs xor ah,ah):
;   In two places the code needs AH=0 to zero-extend AL into AX before
;   a multiply-by-16 shift.  AL holds a partition index (always 0-3),
;   so it is never negative and CBW (sign-extend AL into AX) produces
;   the same result as XOR AH,AH.  CBW encodes in 1 byte vs 2 for XOR.
;   The byte saved in wait_key was spent on the explicit MOV AH,0Eh
;   before the '>' prompt (see below).
;
; AH undefined after INT 10h/0Eh:
;   The BIOS specification leaves AH undefined on return from INT 10h/0Eh
;   (teletype output).  Relying on AH surviving the call is fragile and
;   breaks on some real and emulated BIOSes.  Every INT 10h call in this
;   code is preceded by an explicit MOV AH,0Eh.  The '>' prompt after
;   display_list therefore sets AH explicitly rather than assuming it
;   survives from the last call inside display_list.  The 2-byte cost of
;   MOV AH,0Eh was funded by the 1-byte cbw saving plus the last zero
;   pad byte, leaving the binary with no slack at all.
;
; Stack placement:
;   SP is initialised to 0x6000 — the base address of our relocated copy.
;   The 8088 stack grows downward, so pushes immediately begin writing
;   below 0x6000 into the unused region 0x5FFE, 0x5FFC, ... well away
;   from both the IVT (0x0000) and the relocated code (0x6000-0x61FF).
;   This gives roughly 22 KB of safe stack space.
;
; Relocation delta:
;   ORG is 0x7C00 so every label assembles to a 0x7Cxx address.
;   After relocation the bytes live at 0x6000+offset.
;   RDELTA (= 0x6000 - 0x7C00 = -0x1C00) is added to every label
;   referenced after the far-jump so the reference lands in the
;   relocated copy.
;
; Far jumps:
;   Two far jumps appear in this code, both encoded as raw bytes rather
;   than using the NASM 'jmp seg:off' mnemonic.  This is necessary
;   because NASM's far jump syntax does not accept arithmetic expressions
;   in the offset field — only simple labels or literal values.  The raw
;   encoding (0xEA followed by offset word and segment word) is the
;   standard workaround used in MBR and bootloader code.
;
; Binary layout (512 bytes):
;   0x000-0x002  Near jump over data area (E9 xx xx)
;   0x003-0x00A  str_boot_from: "Boot:\r\n\0" (8 bytes)
;   0x00B-0x01F  str_floppy_ab: "A Floppy\r\nB Floppy\r\n\0" (21 bytes)
;   0x020-0x02C  str_disk_err: "Read error\r\n\0" (13 bytes)
;   0x02D-0x03F  str_no_boot: ": No boot record\r\n\0" (19 bytes)
;   0x040        scratch_char: display char of last selected device (1 byte)
;   0x041-0x080  part_labels: four 16-byte label slots, zeroed at build time
;   0x081-0x1AF  Boot code (instructions)
;   0x1B0        Zero padding (1 byte code slack — absorbs single-byte code growth)
;   0x1B1        0xD9 — Cassini commemorative byte (see note below)
;   0x1B2-0x1B6  'mbr88' signature (5 bytes: 6D 62 72 38 38)
;   0x1B7        Version byte: high nibble = major, low nibble = minor (0x02 = v0.2)
;   0x1B8-0x1BB  NT/OS2 disk signature region (left clear — not used by mbr88)
;   0x1BC-0x1BD  NT/OS2 reserved word (left clear)
;   0x1BE-0x1FD  Partition table (4 x 16-byte entries)
;   0x1FE-0x1FF  Boot signature 55h AAh
;
; Cassini commemorative byte (0x1B1 = 0xD9):
;   "So on Sept. 15th 2017 I was up all night nursing along a packet parser 
;    for the RPWS instrument on the Cassini spacecraft. The mission was almost
;    over and it was time for us to plunge Cassini into Saturn's atmosphere 
;    while we still had enough fuel to change orbits.
;
;    A week before, our collegues over in Sweeden wanted to have a Cassini 
;    Crash Party complete with live data feeds, problem was, they could only
;    read files, and our packet parser only flushed files every 30 minutes.
;    That's kinda slow for a party display so I stopped the autoprocessor and
;    stayed up all night kicking off file creation every 5 minutes, and
;    watching for errors. There was little to do but watch packets go by as
;    Cassini barrelled towards Saturn.
;
;    Finally sometime around 4 AM, the telemetry stream just stopped. I checked
;    our socket to the Deep Space Network. It was good, but there were no more
;    packets. So I ran a hex dump on the last partal packet and doubled over 
;    laughing... in an empty building... in the middle of the night.
;
;    The last byte was 0xD9 
;
;    Cassini had punked us. 
;
;    After this byte we were absolutely "Dee-Nined" any more data :-)
;    "
;  
;    - C. Piker, MBR88 developer
; =============================================================================

        bits    16
        org     7C00h

RDELTA  equ     6000h - 7C00h           ; = -0x1C00

; =============================================================================
; String and data table
; =============================================================================

        jmp     near start              ; 3-byte near jump over data area

str_boot_from:
        db      'Boot:', 0Dh, 0Ah, 0    ; "Boot:\r\n\0"  8 bytes

str_floppy_ab:
        db      'A Floppy', 0Dh, 0Ah   ; "A Floppy\r\n"
        db      'B Floppy', 0Dh, 0Ah, 0 ; "B Floppy\r\n\0"  21 bytes total

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
;   Bytes  0-10  Label text (1 to 11 printable ASCII characters, 0x20-0x7E)
;   Bytes 11-12  CR + LF (0x0D 0x0A) — may appear earlier if label is short
;   Byte   13    Null terminator (stops print_str)
;   Bytes 14-15  Zero padding
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
        mov     sp, 6000h               ; stack grows down from 0x6000 into free low memory
        sti

; =============================================================================
; RELOCATION — copy 512 bytes from 0x7C00 to 0x6000, then far jump there.
;
; The far jump is encoded as raw bytes (0xEA + offset word + segment word)
; because NASM's far jump syntax does not accept arithmetic expressions in
; the offset field.  The target is after_reloc in the relocated copy:
;   offset = 0x6000 + (after_reloc - 0x7C00)
;   segment = 0x0000
; =============================================================================
relocate:
        mov     si, 7C00h
        mov     di, 6000h
        mov     cx, 256
        rep     movsw

        db      0EAh                            ; far jump opcode
        dw      6000h + (after_reloc - 7C00h)  ; offset in relocated copy
        dw      0000h                           ; segment

; =============================================================================
; Execution continues here inside the RELOCATED copy at 0x6000.
; =============================================================================
after_reloc:

; Print header, then the menu list, then the prompt
        mov     si, str_boot_from + RDELTA  ; "Boot:\r\n"
        call    print_str

        call    display_list            ; floppy lines + partition lines

        ; Print the ">" prompt.
        ; BH=00h is preserved across INT 10h/0Eh (page number, never modified).
        ; AH is undefined on return from INT 10h/0Eh on some BIOSes, so set
        ; it explicitly.  The byte cost is funded by cbw in wait_key (below).
        mov     ah, 0Eh
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
        mov     bl, al                  ; save index in BL — cbw below will zero AH,
                                        ; leaving BL as the only intact copy of the index

        cbw                             ; zero-extend AL into AX (AL is 0-3, never negative,
                                        ; so CBW = XOR AH,AH here but costs 1 byte instead of 2;
                                        ; the saved byte was spent on MOV AH,0Eh for the '>' prompt)
        mov     cl, 4
        shl     ax, cl                  ; AX = index * 16
        add     ax, part_table + RDELTA
        mov     si, ax

        cmp     byte [si], 80h          ; only accept bootable (0x80) partitions;
        jne     wait_key                ; status 0x00 covers both empty and non-bootable slots

        ; Save chosen digit char for error reporting (bad_vbr prints it).
        ; No explicit echo here — bad_vbr echoes scratch_char on failure,
        ; and on success the pre-VBR CR+LF fires cleanly without a digit.
        ; INT 16h/00h does not echo keystrokes on PC BIOS.
        mov     al, bl
        add     al, '1'                 ; convert 0-based index back to digit char
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
        ; Set AL = 'A' or 'B' for scratch_char (bad_vbr prints it on failure).
        ; No explicit echo — same reasoning as wait_key above.
        mov     al, 'A'
        cmp     dl, 01h
        jne     .set_scratch
        mov     al, 'B'
.set_scratch:
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
;
; The far jump to the VBR is encoded as raw bytes (0xEA + 0x7C00 + 0x0000)
; because NASM's far jump syntax does not accept arithmetic expressions in
; the offset field — though in this case the target is a literal 0x7C00
; and raw encoding is used here for consistency with the relocation jump.
;
; Retry loop: floppy drives require up to 3 attempts to allow the disk
; motor time to reach stable speed.  The reset is retried each pass.
; If the reset itself fails we proceed to the read anyway — a failed reset
; will produce a failed read, handled by the retry loop.  After 3 consecutive
; read failures we report disk_error.  This follows the convention of most
; XT-class MBR boot blocks.
;
; Register allocation inside do_chs_read:
;   BL  drive number — saved here because INT 13h/00h reset may corrupt DL.
;   DI  CHS fields (CX on entry) — callers load CL=sector|cyl-hi, CH=cyl-lo
;       into CX before jumping here.  CX is immediately reused as the retry
;       counter (MOV CX,3), so CHS is saved to DI first.  Before each read
;       call CX is temporarily swapped back to CHS (PUSH CX / MOV CX,DI),
;       and the counter is restored from the stack after INT 13h (POP CX).
;       DI is not touched by INT 13h on XT BIOS.
; =============================================================================
do_chs_read:
        mov     bl, dl                  ; save drive number across INT 13h reset
        mov     di, cx                  ; save CHS — CX reused as retry counter below
        mov     cx, 3                   ; retry counter (3 attempts)

.retry:
        mov     ah, 00h                 ; INT 13h/00h = reset
        int     13h
        ; jc not checked after reset — a reset failure produces a read
        ; failure on the next instruction, caught by the retry loop.

        mov     dl, bl                  ; restore drive number (BIOS may corrupt)
        push    cx                      ; save retry counter — CX needed for CHS
        mov     cx, di                  ; restore CHS fields for INT 13h/02h

        mov     ah, 02h                 ; INT 13h/02h = read sectors
        mov     al, 01h
        mov     bx, 7C00h               ; ES:BX = read buffer (ES=0 set at startup)
        int     13h
        pop     cx                      ; restore retry counter
        jnc     .read_ok                ; success — proceed to VBR check
        loop    .retry                  ; read failed — decrement CX, retry
        jmp     disk_error              ; all 3 attempts failed
.read_ok:

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

        ; Far-jump to VBR at 0x0000:0x7C00 — DL = boot drive (IBM BIOS convention).
        ; Encoded as raw bytes for consistency with the relocation jump above.
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
;   N <label>\r\n   (for each bootable partition, N = '1'..'4')
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
        mov     si, part_table + RDELTA ; SI -> first partition entry
        mov     bl, '1'                 ; starting digit
        mov     cx, 4                   ; exactly 4 entries

.part_loop:
        cmp     byte [si], 80h          ; only show bootable (0x80) partitions;
        jne     .next                   ; status 0x00 covers both empty and non-bootable slots

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
        cbw                             ; zero-extend AL into AX (AL is 0-3; CBW = XOR AH,AH here,
                                        ; 1 byte vs 2 — see file header note on cbw byte budget)
        mov     cl, 4
        shl     ax, cl                  ; AX = index * 16
        add     ax, part_labels + RDELTA
        mov     si, ax
        call    print_str               ; print label (contains \r\n\0)

        ; Recalculate SI to current partition entry (CL still 4, CX still pushed)
        mov     al, bl
        sub     al, '1'
        cbw                             ; zero-extend AL into AX (CBW = XOR AH,AH, 1 byte vs 2)
        shl     ax, cl                  ; AX = index * 16  (CL still = 4)
        add     ax, part_table + RDELTA
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
; Tail area — slack, Cassini byte, mbr88 signature, version, NT region.
;
; Layout from 0x1B0:
;   0x1B0        Zero padding (1 byte code slack — absorbs single-byte growth)
;   0x1B1        0xD9 — Cassini commemorative byte
;   0x1B2-0x1B6  'mbr88' signature (5 bytes)
;   0x1B7        Version byte 0x02 (high nibble=major=0, low nibble=minor=2)
;   0x1B8-0x1BB  NT/OS2 disk signature region (zeroed — not used by mbr88)
;   0x1BC-0x1BD  NT/OS2 reserved word (zeroed)
;   0x1BE        Partition table begins
;
; The NT disk signature region (0x1B8-0x1BB) is intentionally left clear.
; Windows NT and OS/2 write a random 32-bit disk identifier here.  Occupying
; this region (as v0.1 did) prevents those OSes from installing cleanly on
; the same disk.  Moving the mbr88 signature to 0x1B2 frees the NT region
; entirely, improving compatibility with 286-class hardware and NT-family OSes
; should this MBR ever be used beyond XT-class targets.
; =============================================================================

        times   1B1h - ($ - $$)  db 0  ; code slack (1 byte at 0x1B0)

        db      0D9h                    ; 0x1B1 — Cassini: last byte from Saturn, 2017-09-15

mbr_sig:
        db      'mbr88'                 ; 0x1B2-0x1B6 — 5-byte signature: 6D 62 72 38 38

mbr_ver:
        db      02h                     ; 0x1B7 — version 0.2 (major=0, minor=2)

        ; NT disk signature region and reserved word — must remain zeroed.
        ; times expression pads from 0x1B8 through 0x1BD (6 bytes).
        times   1BEh - ($ - $$)  db 0

part_table:
        times   64  db 0                ; 0x1BE-0x1FD — written by mbrpatch

        times   510 - ($ - $$)  db 0
        db      55h
        db      0AAh
