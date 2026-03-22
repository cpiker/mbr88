/*
 * mbr_patch.c — Interactive IBM XT-style MBR partition table editor
 *
 * Written by Claude (Anthropic) and verified by Chris Piker.
 *
 * Purpose:
 *   Created to support dual booting FreeDOS and ELKS Linux on IBM XT class
 *   hardware alongside the mbr88_nasm.asm / mbr88_gas.s boot records.
 *   Equivalent to mbr_patch.py but written in C99 for use on ELKS Linux
 *   and other environments without a Python interpreter.
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
 *   Thanks to osdev.org for the reference material and community insights
 *   that informed the design of the mbr88 boot records this tool supports.
 *   https://wiki.osdev.org/MBR_(x86)
 *
 * Build (cross-compile for ELKS):
 *   ia16-elf-gcc -melks -Os -o mbr_patch mbr_patch.c
 *
 * Build (native Linux, for testing):
 *   gcc -std=c99 -Wall -o mbr_patch mbr_patch.c
 *
 * Design notes for ELKS:
 *   - No malloc/heap: all buffers are static globals or small stack locals.
 *   - No floating point: sizes use integer KB/MB arithmetic.
 *   - Explicit unsigned char/unsigned long for clarity on 16-bit targets.
 *   - File I/O uses POSIX open/read/write/close.
 *   - Global variables used freely as permitted for this codebase.
 *
 * IBM XT / CHS partition entry layout (16 bytes, little-endian):
 *   Byte  0      Status          0x00=inactive, 0x80=bootable
 *   Bytes 1-3    CHS first sector  H, S|((C>>8)<<6), C&0xFF
 *   Byte  4      Partition type
 *   Bytes 5-7    CHS last sector
 *   Bytes 8-11   LBA start (32-bit little-endian)
 *   Bytes 12-15  LBA size  (32-bit little-endian)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define MBR_SIZE          512
#define PTABLE_OFFSET     0x1BE
#define ENTRY_SIZE        16
#define NUM_ENTRIES       4
#define BOOTSIG_OFFSET    0x1FE

#define MBR88_SIG_OFFSET  0x1B9
#define MBR88_SIG_LEN     5

#define LABEL_BASE        0x43
#define LABEL_SLOT_SZ     16
#define LABEL_MAX         11

/* Two-column display geometry — 38 + 4 + 38 = 80 */
#define COL_WIDTH         38
#define COL_GAP           4
#define LINE_WIDTH        (COL_WIDTH * 2 + COL_GAP)

#define DEFAULT_HEADS     16
#define DEFAULT_SECTORS   17

static const char MBR88_SIG[MBR88_SIG_LEN] = {'m','b','r','8','8'};

/* -------------------------------------------------------------------------
 * mbr88 blank template — included from detached header
 * -----------------------------------------------------------------------*/

#include "mbr88_template.h"

/* Catch any future mismatch between the two size constants at compile time */
typedef char mbr_size_check_[
    (MBR88_TEMPLATE_SIZE == MBR_SIZE) ? 1 : -1
];

/* -------------------------------------------------------------------------
 * Session state globals
 * -----------------------------------------------------------------------*/

static unsigned char mbr[MBR_SIZE];
static char          mbr_path[256];
static int           file_exists;
static int           has_mbr88_sig;
static int           dirty;
static int           geo_heads;      /* 0 = not set yet */
static int           geo_sectors;

/* -------------------------------------------------------------------------
 * Partition type table
 * -----------------------------------------------------------------------*/

typedef struct { unsigned char type; const char *name; } TypeName;

static const TypeName TYPE_NAMES[] = {
    { 0x00, "Empty"         },
    { 0x01, "FAT12"         },
    { 0x04, "FAT16 <32MB"  },
    { 0x05, "Extended"      },
    { 0x06, "FAT16B >=32MB"},
    { 0x0B, "FAT32"         },
    { 0x0C, "FAT32 LBA"     },
    { 0x80, "MINIX old"     },
    { 0x81, "MINIX"         },
    { 0x82, "Linux swap"    },
    { 0x83, "Linux"         },
    { 0x00, NULL            }
};

static const char *type_name(unsigned char t)
{
    int i;
    for (i = 0; TYPE_NAMES[i].name; i++)
        if (TYPE_NAMES[i].type == t)
            return TYPE_NAMES[i].name;
    return "Unknown";
}

static void print_type_hints(void)
{
    int i;
    for (i = 0; TYPE_NAMES[i].name; i++)
        printf("    0x%02X  %s\n", TYPE_NAMES[i].type, TYPE_NAMES[i].name);
}

/* -------------------------------------------------------------------------
 * CHS helpers
 * -----------------------------------------------------------------------*/

static void pack_chs(unsigned char out[3], int cyl, int head, int sector)
{
    out[0] = (unsigned char)(head & 0xFF);
    out[1] = (unsigned char)((sector & 0x3F) | ((cyl >> 2) & 0xC0));
    out[2] = (unsigned char)(cyl & 0xFF);
}

static void unpack_chs(const unsigned char in[3],
                       int *cyl, int *head, int *sector)
{
    *head   = in[0];
    *sector = in[1] & 0x3F;
    *cyl    = ((in[1] & 0xC0) << 2) | in[2];
}

static unsigned long chs_to_lba(int cyl, int head, int sector)
{
    return ((unsigned long)cyl * geo_heads + head)
           * geo_sectors + (sector - 1);
}

/* -------------------------------------------------------------------------
 * Label slot helpers
 * -----------------------------------------------------------------------*/

static void build_label_slot(unsigned char slot[LABEL_SLOT_SZ],
                              const char *text)
{
    int len = (int)strlen(text);
    if (len > LABEL_MAX) len = LABEL_MAX;
    memset(slot, 0, LABEL_SLOT_SZ);
    memcpy(slot, text, len);
    slot[len]   = '\r';
    slot[len+1] = '\n';
    slot[len+2] = '\0';
}

/*
 * Copy the label for slot_0based into buf (must be LABEL_MAX+1 bytes).
 * Strips trailing CR/LF/spaces.
 */
static void read_label(int slot_0based, char buf[LABEL_MAX + 1])
{
    int off = LABEL_BASE + slot_0based * LABEL_SLOT_SZ;
    int len;
    memcpy(buf, mbr + off, LABEL_MAX);
    buf[LABEL_MAX] = '\0';
    len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == ' '  || buf[len-1] == '\t' ||
                       buf[len-1] == '\r' || buf[len-1] == '\n'))
        buf[--len] = '\0';
}

static void write_label(int slot_0based, const char *text)
{
    unsigned char slot[LABEL_SLOT_SZ];
    build_label_slot(slot, text);
    memcpy(mbr + LABEL_BASE + slot_0based * LABEL_SLOT_SZ, slot, LABEL_SLOT_SZ);
}

/* -------------------------------------------------------------------------
 * mbr88 detection and upgrade
 * -----------------------------------------------------------------------*/

static int detect_mbr88(void)
{
    return memcmp(mbr + MBR88_SIG_OFFSET, MBR88_SIG, MBR88_SIG_LEN) == 0;
}

static void upgrade_to_mbr88(void)
{
    unsigned char old_ptable[64];
    memcpy(old_ptable, mbr + PTABLE_OFFSET, 64);
    memcpy(mbr, MBR88_TEMPLATE, MBR_SIZE);
    memcpy(mbr + PTABLE_OFFSET, old_ptable, 64);
}

/* -------------------------------------------------------------------------
 * File I/O
 * -----------------------------------------------------------------------*/

static int copy_file(const char *src, const char *dst)
{
    unsigned char buf[512];
    int fdin, fdout, n, result = 0;
    fdin = open(src, O_RDONLY);
    if (fdin < 0) { perror(src); return -1; }
    fdout = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fdout < 0) { perror(dst); close(fdin); return -1; }
    while ((n = read(fdin, buf, sizeof(buf))) > 0) {
        if (write(fdout, buf, n) != n) { perror(dst); result = -1; break; }
    }
    close(fdin);
    close(fdout);
    return result;
}

/* -------------------------------------------------------------------------
 * Input helpers
 * -----------------------------------------------------------------------*/

static int read_line(char *buf, int len)
{
    int n;
    if (!fgets(buf, len, stdin)) return 0;
    n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';
    return 1;
}

static int ask_int(const char *prompt, int lo, int hi)
{
    char buf[16];
    int val;
    for (;;) {
        printf("%s", prompt); fflush(stdout);
        if (!read_line(buf, sizeof(buf))) { fprintf(stderr,"EOF\n"); exit(1); }
        val = atoi(buf);
        if (val >= lo && val <= hi) return val;
        printf("  Please enter a value between %d and %d.\n", lo, hi);
    }
}

static int ask_hex(const char *prompt, int lo, int hi)
{
    char buf[16];
    int val;
    for (;;) {
        printf("%s", prompt); fflush(stdout);
        if (!read_line(buf, sizeof(buf))) { fprintf(stderr,"EOF\n"); exit(1); }
        val = (int)strtol(buf, NULL, 0);
        if (val >= lo && val <= hi) return val;
        printf("  Please enter a value between 0x%02X and 0x%02X.\n", lo, hi);
    }
}

static int ask_yn(const char *prompt)
{
    char buf[8];
    for (;;) {
        printf("%s", prompt); fflush(stdout);
        if (!read_line(buf, sizeof(buf))) { fprintf(stderr,"EOF\n"); exit(1); }
        if (buf[0]=='y'||buf[0]=='Y') return 1;
        if (buf[0]=='n'||buf[0]=='N') return 0;
        printf("  Please press Y or N.\n");
    }
}

static int ask_slot(const char *prompt)
{
    char buf[8];
    printf("%s", prompt); fflush(stdout);
    if (!read_line(buf, sizeof(buf))) return -1;
    if (buf[0]>='1' && buf[0]<='4' && buf[1]=='\0') return buf[0]-'0';
    printf("  Please enter a partition number 1-4.\n");
    return -1;
}

/* -------------------------------------------------------------------------
 * Two-column display helpers
 * -----------------------------------------------------------------------*/

static int is_empty(int slot_0based)
{
    int off = PTABLE_OFFSET + slot_0based * ENTRY_SIZE;
    const unsigned char *e = mbr + off;
    return (e[4]==0
            && e[1]==0 && e[2]==0 && e[3]==0
            && e[5]==0 && e[6]==0 && e[7]==0);
}

/*
 * Format a size string from a sector count into buf (at least 24 bytes).
 * No floating point.
 */
static void fmt_size(char *buf, unsigned long sectors)
{
    unsigned long b = sectors * 512UL;
    if (b >= 1024UL*1024UL)
        sprintf(buf, "%lu MB", b / (1024UL*1024UL));
    else if (b >= 1024UL)
        sprintf(buf, "%lu KB", b / 1024UL);
    else
        sprintf(buf, "%lu B", b);
}

/*
 * Fill lines[0..6] with COL_WIDTH-wide strings for one partition column.
 * Always produces exactly 7 lines so the two columns zip cleanly.
 */

/* Write a left-padded string into one column line. */
static void col_row(char line[COL_WIDTH+1], const char *s)
{
    snprintf(line, COL_WIDTH+1, "%-*.*s", COL_WIDTH, COL_WIDTH, s);
}

static void col_lines(int slot_1based, char lines[7][COL_WIDTH + 1])
{
    int i = slot_1based - 1;
    int n;
    char tmp[64];   /* large enough for any formatted column row */

    if (is_empty(i)) {
        snprintf(tmp, sizeof(tmp), " Partition %d", slot_1based);
        col_row(lines[0], tmp);
        col_row(lines[1], "  <empty>");
        for (n = 2; n < 7; n++) col_row(lines[n], "");
        return;
    }

    {
        int off = PTABLE_OFFSET + i * ENTRY_SIZE;
        const unsigned char *e = mbr + off;
        unsigned char status    = e[0];
        unsigned char part_type = e[4];
        int cyl_s, head_s, sec_s;
        int cyl_e, head_e, sec_e;
        unsigned long lba_size;
        char size_buf[24];
        char label_buf[LABEL_MAX + 1];
        const char *tname    = type_name(part_type);
        const char *boot_tag = (status == 0x80) ? "Boot" : "    ";

        unpack_chs(e+1, &cyl_s, &head_s, &sec_s);
        unpack_chs(e+5, &cyl_e, &head_e, &sec_e);
        lba_size = (unsigned long)e[12]
                 | ((unsigned long)e[13] << 8)
                 | ((unsigned long)e[14] << 16)
                 | ((unsigned long)e[15] << 24);
        fmt_size(size_buf, lba_size);

        snprintf(tmp,sizeof(tmp)," Partition %d  [%s]",slot_1based,boot_tag);
        col_row(lines[0], tmp);
        snprintf(tmp,sizeof(tmp),"  Type:  0x%02X %.16s",part_type,tname);
        col_row(lines[1], tmp);

        if (has_mbr88_sig) {
            read_label(i, label_buf);
            snprintf(tmp, sizeof(tmp), "  Label: %.26s", label_buf);
            col_row(lines[2], tmp);
        } else {
            col_row(lines[2], "");
        }

        snprintf(tmp,sizeof(tmp),"  Start: C=%-4d H=%-3d S=%d",
                 cyl_s, head_s, sec_s);
        col_row(lines[3], tmp);
        snprintf(tmp,sizeof(tmp),"  End:   C=%-4d H=%-3d S=%d",
                 cyl_e, head_e, sec_e);
        col_row(lines[4], tmp);

        /* Size: abbreviate "sectors" to fit in 38 chars */
        snprintf(tmp, sizeof(tmp), "  Size:  %s (%lu sec)", size_buf, lba_size);
        col_row(lines[5], tmp);
        col_row(lines[6], "");
    }
}

static void print_table(void)
{
    char left[7][COL_WIDTH+1];
    char right[7][COL_WIDTH+1];
    char gap[COL_GAP+1];
    char title[LINE_WIDTH+1];
    const char *sig_tag, *geo_tag_fmt;
    char geo_buf[32];
    int n;

    memset(gap, ' ', COL_GAP);
    gap[COL_GAP] = '\0';

    /* Header */
    for (n = 0; n < LINE_WIDTH; n++) putchar('=');
    putchar('\n');

    if (dirty)
        snprintf(title, sizeof(title), "MBR Partition Table  (* unsaved changes)");
    else
        snprintf(title, sizeof(title), "MBR Partition Table");
    /* Centre the title */
    {
        int len   = (int)strlen(title);
        int pad   = (LINE_WIDTH - len) / 2;
        int rpad  = LINE_WIDTH - len - pad;
        printf("%*s%s%*s\n", pad, "", title, rpad, "");
    }

    for (n = 0; n < LINE_WIDTH; n++) putchar('=');
    putchar('\n');

    /* Top row: partitions 1 and 2 */
    col_lines(1, left);
    col_lines(2, right);
    for (n = 0; n < 7; n++)
        printf("%s%s%s\n", left[n], gap, right[n]);

    for (n = 0; n < LINE_WIDTH; n++) putchar('-');
    putchar('\n');

    /* Bottom row: partitions 3 and 4 */
    col_lines(3, left);
    col_lines(4, right);
    for (n = 0; n < 7; n++)
        printf("%s%s%s\n", left[n], gap, right[n]);

    for (n = 0; n < LINE_WIDTH; n++) putchar('=');
    putchar('\n');

    /* Status bar */
    sig_tag = has_mbr88_sig ? "[mbr88]" : "[generic MBR]";
    if (geo_heads) {
        snprintf(geo_buf, sizeof(geo_buf), "Geometry: %dH/%dS",
                 geo_heads, geo_sectors);
        geo_tag_fmt = geo_buf;
    } else {
        geo_tag_fmt = "Geometry:";
    }
    /* Extract basename from mbr_path */
    {
        const char *base = strrchr(mbr_path, '/');
        base = base ? base + 1 : mbr_path;
        printf("  File: %s  %s  %s\n\n", base, sig_tag, geo_tag_fmt);
    }
}

/* -------------------------------------------------------------------------
 * Command implementations
 * -----------------------------------------------------------------------*/

static void cmd_geometry(void)
{
    printf("\n");
    printf("Drive geometry tells the tool how to convert CHS addresses into\n");
    printf("LBA sector numbers for the partition table entries.\n\n");
    printf("IBM XT default: %d heads per cylinder, %d sectors per track.\n",
           DEFAULT_HEADS, DEFAULT_SECTORS);
    printf("Check your drive specification or use values reported by fdisk.\n\n");

    if (ask_yn("Use IBM XT default geometry (16H / 17S)? (Y/N): ")) {
        geo_heads   = DEFAULT_HEADS;
        geo_sectors = DEFAULT_SECTORS;
    } else {
        geo_heads   = ask_int("  Heads per cylinder (1-255): ", 1, 255);
        geo_sectors = ask_int("  Sectors per track  (1-63):  ", 1, 63);
    }
    printf("  Geometry set: %d heads, %d sectors/track.\n",
           geo_heads, geo_sectors);
}

static void cmd_new(void)
{
    int slot, i, off;
    int cyl_s, head_s, sec_s;
    int cyl_e, head_e, sec_e;
    int part_type, bootable;
    unsigned long lba_start, lba_end, lba_size;
    unsigned char entry[ENTRY_SIZE];

    if (!geo_heads) {
        printf("  Geometry not set — run 'g' first.\n");
        return;
    }

    slot = ask_slot("  Partition number (1-4): ");
    if (slot < 0) return;
    i   = slot - 1;
    off = PTABLE_OFFSET + i * ENTRY_SIZE;

    if (!is_empty(i)) {
        if (!ask_yn("  Partition is not empty. Redefine it? (Y/N): "))
            return;
    }

    printf("\n  -- Partition %d: Starting CHS --\n", slot);
    cyl_s  = ask_int("    Cylinder (0-1023): ", 0, 1023);
    head_s = ask_int("    Head     (0-255):  ", 0, 255);
    sec_s  = ask_int("    Sector   (1-63):   ", 1, 63);

    printf("\n  -- Partition %d: Ending CHS --\n", slot);
    cyl_e  = ask_int("    Cylinder (0-1023): ", 0, 1023);
    head_e = ask_int("    Head     (0-255):  ", 0, 255);
    sec_e  = ask_int("    Sector   (1-63):   ", 1, 63);

    printf("\n  Common partition types:\n");
    print_type_hints();
    part_type = ask_hex("  Partition type (hex or decimal): ", 0x00, 0xFF);
    bootable  = ask_yn("  Mark as bootable? (Y/N): ");

    lba_start = chs_to_lba(cyl_s, head_s, sec_s);
    lba_end   = chs_to_lba(cyl_e, head_e, sec_e);
    lba_size  = (lba_end >= lba_start) ? lba_end - lba_start + 1 : 0;

    memset(entry, 0, ENTRY_SIZE);
    entry[0] = (unsigned char)(bootable ? 0x80 : 0x00);
    pack_chs(entry+1, cyl_s, head_s, sec_s);
    entry[4] = (unsigned char)part_type;
    pack_chs(entry+5, cyl_e, head_e, sec_e);
    entry[8]  = (unsigned char)(lba_start & 0xFF);
    entry[9]  = (unsigned char)((lba_start >>  8) & 0xFF);
    entry[10] = (unsigned char)((lba_start >> 16) & 0xFF);
    entry[11] = (unsigned char)((lba_start >> 24) & 0xFF);
    entry[12] = (unsigned char)(lba_size & 0xFF);
    entry[13] = (unsigned char)((lba_size >>  8) & 0xFF);
    entry[14] = (unsigned char)((lba_size >> 16) & 0xFF);
    entry[15] = (unsigned char)((lba_size >> 24) & 0xFF);

    memcpy(mbr + off, entry, ENTRY_SIZE);
    dirty = 1;
    printf("  Partition %d defined.\n", slot);
}

static void cmd_delete(void)
{
    int slot, off;

    slot = ask_slot("  Partition number to delete (1-4): ");
    if (slot < 0) return;

    if (is_empty(slot - 1)) {
        printf("  Partition %d is already empty.\n", slot);
        return;
    }

    if (!ask_yn("  Delete partition? This zeros all 16 bytes of the entry "
                "and its label. (Y/N): "))
        return;

    off = PTABLE_OFFSET + (slot - 1) * ENTRY_SIZE;
    memset(mbr + off, 0, ENTRY_SIZE);

    if (has_mbr88_sig)
        memset(mbr + LABEL_BASE + (slot-1) * LABEL_SLOT_SZ, 0, LABEL_SLOT_SZ);

    dirty = 1;
    printf("  Partition %d deleted.\n", slot);
}

static void cmd_set_type(void)
{
    int slot, part_type;

    slot = ask_slot("  Partition number (1-4): ");
    if (slot < 0) return;

    if (is_empty(slot - 1)) {
        printf("  Partition %d is empty — use 'n' to define it first.\n", slot);
        return;
    }

    printf("\n  Common partition types:\n");
    print_type_hints();
    part_type = ask_hex("  New partition type (hex or decimal): ", 0x00, 0xFF);

    mbr[PTABLE_OFFSET + (slot-1) * ENTRY_SIZE + 4] = (unsigned char)part_type;
    dirty = 1;
    printf("  Partition %d type set to 0x%02X.\n", slot, part_type);
}

static void cmd_bootable(void)
{
    int slot, off;
    unsigned char current, new_val;

    slot = ask_slot("  Partition number (1-4): ");
    if (slot < 0) return;

    if (is_empty(slot - 1)) {
        printf("  Partition %d is empty — use 'n' to define it first.\n", slot);
        return;
    }

    off     = PTABLE_OFFSET + (slot-1) * ENTRY_SIZE;
    current = mbr[off];
    new_val = (current == 0x80) ? 0x00 : 0x80;
    mbr[off] = new_val;
    dirty = 1;
    printf("  Partition %d is now %s (0x%02X).\n", slot,
           new_val == 0x80 ? "Bootable" : "Inactive", new_val);
    printf("  Note: mbr88 uses the boot menu selection rather than this flag,\n");
    printf("  but other MBR loaders may depend on it.\n");
}

static void cmd_label(void)
{
    int  slot;
    char label_buf[LABEL_MAX + 2];
    char current[LABEL_MAX + 1];

    if (!has_mbr88_sig) {
        printf("  Labels are only supported on mbr88 images.\n");
        printf("  Use -u when starting the program to upgrade this MBR to mbr88.\n");
        return;
    }

    slot = ask_slot("  Partition number (1-4): ");
    if (slot < 0) return;

    if (is_empty(slot - 1)) {
        printf("  Partition %d is empty — use 'n' to define it first.\n", slot);
        return;
    }

    read_label(slot - 1, current);
    printf("  Current label: '%s'\n", current);

    for (;;) {
        printf("  New label (up to %d chars): ", LABEL_MAX);
        fflush(stdout);
        if (!read_line(label_buf, sizeof(label_buf))) { exit(1); }
        if ((int)strlen(label_buf) <= LABEL_MAX) break;
        printf("  Label too long — maximum %d characters.\n", LABEL_MAX);
    }

    if (label_buf[0] == '\0') {
        label_buf[0] = ' ';
        label_buf[1] = '\0';
    }

    write_label(slot - 1, label_buf);
    dirty = 1;
    printf("  Partition %d label set to '%s'.\n", slot, label_buf);
}

static void cmd_types(void)
{
    printf("\n  Common partition type codes:\n");
    print_type_hints();
    putchar('\n');
}

static void cmd_write(void)
{
    int fd, n, active, i;
    char bak[260];

    if (!dirty) {
        printf("  No changes to write.\n");
        return;
    }

    print_table();
    if (!ask_yn("Write changes to disk? (Y/N): ")) {
        printf("  Write cancelled.\n");
        return;
    }

    /* Backup immediately before writing */
    if (file_exists) {
        snprintf(bak, sizeof(bak), "%s.bak", mbr_path);
        if (copy_file(mbr_path, bak) == 0)
            printf("  Backup written to: %s\n", bak);
        else {
            fprintf(stderr, "Error: could not write backup.\n");
            return;
        }
    }

    fd = open(mbr_path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) { perror(mbr_path); return; }
    n = write(fd, mbr, MBR_SIZE);
    close(fd);
    if (n != MBR_SIZE) {
        fprintf(stderr, "Error: short write.\n");
        return;
    }

    active = 0;
    for (i = 0; i < NUM_ENTRIES; i++)
        if (mbr[PTABLE_OFFSET + i * ENTRY_SIZE + 4] != 0)
            active++;

    printf("  Written to '%s'.\n", mbr_path);
    printf("  %d active entries, %d empty slots.\n", active, NUM_ENTRIES-active);
    dirty       = 0;
    file_exists = 1;
}

static void cmd_help(void)
{
    printf("\n  Commands:\n");
    printf("    g  Set drive geometry (required before 'n')\n");
    printf("    n  Define a new partition (or redefine an existing one)\n");
    printf("    d  Delete a partition slot (zeros all 16 bytes + label)\n");
    printf("    s  Set the partition type byte\n");
    printf("    b  Toggle the bootable flag (0x00 / 0x80)\n");
    printf("    l  Set the boot menu label  (mbr88 images only)\n");
    printf("    p  Print the partition table\n");
    printf("    t  List common partition type codes\n");
    printf("    w  Write changes to disk\n");
    printf("    h  This help text\n");
    printf("    q  Quit (prompts if there are unsaved changes)\n");
    putchar('\n');
}

static void print_help(void)
{
    puts("mbr_patch — Interactive IBM XT-style MBR partition table editor");
    puts("");
    puts("Usage:");
    puts("  mbr_patch <mbr_file>");
    puts("  mbr_patch -u <mbr_file>");
    puts("  mbr_patch -h | --help");
    puts("");
    puts("Arguments:");
    puts("  <mbr_file>   Path to a 512-byte MBR binary image to view or edit.");
    puts("               A backup (<mbr_file>.bak) is written immediately");
    puts("               before any changes are committed to disk.");
    puts("");
    puts("Options:");
    puts("  -u           Upgrade mode.  Replace the boot code with the mbr88");
    puts("               boot record while preserving the existing partition");
    puts("               table entries.  Enables full label editing regardless");
    puts("               of what was in the file before.  If <mbr_file> does");
    puts("               not exist it will be created from scratch as a blank");
    puts("               mbr88 image ready for partition entry.");
    puts("  -h, --help   Show this help text and exit.");
    puts("");
    puts("Interactive commands (type 'h' at the prompt for a summary):");
    puts("  g  Set drive geometry       n  New / redefine partition");
    puts("  d  Delete partition         s  Set partition type");
    puts("  b  Toggle bootable flag     l  Set boot menu label");
    puts("  p  Print table              t  List type codes");
    puts("  w  Write to disk            q  Quit");
    puts("");
    puts("mbr88 signature:");
    puts("  If the 5-byte signature 'mbr88' is present at offset 0x1B9,");
    puts("  label editing is enabled.  Without it only the partition table");
    puts("  entries are written; the rest of the MBR is left untouched.");
    puts("  Use -u to upgrade any MBR to mbr88.");
    puts("");
    puts("Examples:");
    puts("  mbr_patch mbr.bin");
    puts("  mbr_patch -u mbr.bin");
    puts("  mbr_patch -u new.bin     (create from scratch)");
}

/* -------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
    int  upgrade_mode = 0;
    int  fd, n;
    char cmd_buf[8];

    if (argc == 2 && (strcmp(argv[1],"-h")==0 || strcmp(argv[1],"--help")==0)){
        print_help();
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "-u") == 0) {
        upgrade_mode = 1;
        argv++; argc--;
    }

    if (argc != 2) {
        fprintf(stderr, "Usage: mbr_patch [-u] <mbr_file>\n");
        fprintf(stderr, "       mbr_patch -h | --help\n");
        return 1;
    }

    strncpy(mbr_path, argv[1], sizeof(mbr_path) - 1);
    mbr_path[sizeof(mbr_path)-1] = '\0';

    /* Try to open the file */
    fd = open(mbr_path, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, mbr, MBR_SIZE);
        close(fd);
        if (n < MBR_SIZE) {
            fprintf(stderr, "Error: '%s' is smaller than 512 bytes.\n", mbr_path);
            return 1;
        }
        file_exists = 1;
    } else if (upgrade_mode) {
        memset(mbr, 0, MBR_SIZE);
        file_exists = 0;
    } else {
        perror(mbr_path);
        return 1;
    }

    if (file_exists && !upgrade_mode
        && (mbr[BOOTSIG_OFFSET] != 0x55 || mbr[BOOTSIG_OFFSET+1] != 0xAA))
        printf("Warning: boot signature at 0x1FE is not 55 AA.\n");

    if (upgrade_mode) {
        upgrade_to_mbr88();
        has_mbr88_sig = 1;
        if (file_exists)
            puts("Upgrade mode: boot code replaced, partition table preserved.");
        else
            printf("Upgrade mode: new mbr88 image '%s' (blank partition table).\n",
                   mbr_path);
        puts("  Use 'l' to set labels, 'w' to write when done.");
    } else {
        has_mbr88_sig = detect_mbr88();
        if (has_mbr88_sig)
            puts("mbr88 boot record — label editing enabled.");
        else {
            puts("Generic MBR — partition table editing only.");
            puts("Use -u to upgrade to mbr88 and enable label editing.");
        }
    }

    print_table();

    /* Command loop */
    for (;;) {
        int redraw = 0;
        printf("Command (h for help): "); fflush(stdout);
        if (!read_line(cmd_buf, sizeof(cmd_buf))) break;
        if (!cmd_buf[0]) continue;

        switch (cmd_buf[0]) {
        case 'q': case 'Q':
            if (dirty && !ask_yn("Unsaved changes — quit without writing? (Y/N): "))
                continue;
            return 0;
        case 'g': case 'G':  cmd_geometry(); redraw=1; break;
        case 'n': case 'N':  cmd_new();      redraw=1; break;
        case 'd': case 'D':  cmd_delete();   redraw=1; break;
        case 's': case 'S':  cmd_set_type(); redraw=1; break;
        case 'b': case 'B':  cmd_bootable(); redraw=1; break;
        case 'l': case 'L':  cmd_label();    redraw=1; break;
        case 'p': case 'P':  print_table();            break;
        case 't': case 'T':  cmd_types();              break;
        case 'w': case 'W':  cmd_write();    redraw=1; break;
        case 'h': case 'H':  cmd_help();               break;
        default:
            printf("  Unknown command '%c'. Type 'h' for help.\n", cmd_buf[0]);
            break;
        }

        if (redraw) print_table();
    }

    return 0;
}
