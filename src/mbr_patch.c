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
 *   - No malloc/heap usage: all buffers are static or on-stack (small).
 *   - No floating point: partition sizes use integer KB/MB arithmetic.
 *   - C99 stdint types avoided in favour of explicit unsigned char /
 *     unsigned short / unsigned long for clarity on a 16-bit target
 *     where int = 16 bits.
 *   - File I/O uses POSIX open/read/write/lseek/close.
 *   - Global variables used freely as permitted for this codebase.
 *   - String I/O uses fgets for input and printf/puts for output.
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

#define MBR_SIZE              512
#define PTABLE_OFFSET         0x1BE
#define ENTRY_SIZE            16
#define NUM_ENTRIES           4
#define BOOTSIG_OFFSET        0x1FE

/* mbr88 signature — 5 ASCII bytes at offset 0x1B9, no null terminator */
#define MBR88_SIG_OFFSET      0x1B9
#define MBR88_SIG_LEN         5
static const char MBR88_SIG[MBR88_SIG_LEN] = {'m','b','r','8','8'};

/* Boot menu label slots written by this tool into mbr88 images */
#define LABEL_BASE_OFFSET     0x43
#define LABEL_SLOT_SIZE       16
#define LABEL_MAX_LEN         11       /* max text chars; slot also holds \r\n\0 */

/* IBM XT default drive geometry */
#define DEFAULT_HEADS         16
#define DEFAULT_SECTORS       17

/* -------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/

static unsigned char mbr[MBR_SIZE];   /* entire MBR held in one static buffer */
static int           has_mbr88_sig;   /* non-zero if mbr88 signature detected */

/* Drive geometry — set once per session, used for all CHS<->LBA conversions */
static int heads_per_cyl;
static int sectors_per_track;

/* -------------------------------------------------------------------------
 * Partition type names
 * -----------------------------------------------------------------------*/

typedef struct { unsigned char type; const char *name; } TypeName;

static const TypeName TYPE_NAMES[] = {
    { 0x00, "Empty"          },
    { 0x01, "FAT12"          },
    { 0x04, "FAT16 <32MB"   },
    { 0x05, "Extended"       },
    { 0x06, "FAT16B >=32MB" },
    { 0x0B, "FAT32"          },
    { 0x0C, "FAT32 LBA"      },
    { 0x80, "MINIX old"      },
    { 0x81, "MINIX"          },
    { 0x82, "Linux swap"     },
    { 0x83, "Linux"          },
    { 0x00, NULL             }   /* sentinel */
};

static const char *type_name(unsigned char t)
{
    int i;
    for (i = 0; TYPE_NAMES[i].name != NULL; i++)
        if (TYPE_NAMES[i].type == t)
            return TYPE_NAMES[i].name;
    return "Unknown";
}

static void print_type_hints(void)
{
    int i;
    for (i = 0; TYPE_NAMES[i].name != NULL; i++)
        printf("  0x%02X  %s\n", TYPE_NAMES[i].type, TYPE_NAMES[i].name);
}

/* -------------------------------------------------------------------------
 * CHS packing / unpacking
 * MBR format: byte0=Head, byte1=Sector[5:0]|Cyl[9:8]<<6, byte2=Cyl[7:0]
 * -----------------------------------------------------------------------*/

static void pack_chs(unsigned char out[3],
                     int cyl, int head, int sector)
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

/* CHS to flat LBA */
static unsigned long chs_to_lba(int cyl, int head, int sector)
{
    return ((unsigned long)cyl * heads_per_cyl + head)
           * sectors_per_track + (sector - 1);
}

/* -------------------------------------------------------------------------
 * Label slot helpers
 * -----------------------------------------------------------------------*/

/*
 * Build a 16-byte label slot from text.
 * Layout: text (up to 11 chars) + \r\n + \0 + zero padding.
 */
static void build_label_slot(unsigned char slot[LABEL_SLOT_SIZE],
                              const char *text)
{
    int len = (int)strlen(text);
    if (len > LABEL_MAX_LEN)
        len = LABEL_MAX_LEN;
    memset(slot, 0, LABEL_SLOT_SIZE);
    memcpy(slot, text, len);
    slot[len]   = '\r';
    slot[len+1] = '\n';
    slot[len+2] = '\0';
}

/*
 * Read and print the label for slot (1-based) from the global mbr buffer.
 * Strips trailing CR, LF, and spaces before printing.
 */
static void print_label(int slot)
{
    int offset = LABEL_BASE_OFFSET + (slot - 1) * LABEL_SLOT_SIZE;
    char buf[LABEL_MAX_LEN + 1];
    int len;

    memcpy(buf, mbr + offset, LABEL_MAX_LEN);
    buf[LABEL_MAX_LEN] = '\0';

    /* strip trailing whitespace and CR/LF */
    len = (int)strlen(buf);
    while (len > 0 &&
           (buf[len-1] == ' '  || buf[len-1] == '\t' ||
            buf[len-1] == '\r' || buf[len-1] == '\n'))
        buf[--len] = '\0';

    printf("    Label:     '%s'\n", buf);
}

/* -------------------------------------------------------------------------
 * Partition entry display
 * -----------------------------------------------------------------------*/

/*
 * Print size in human-readable units without floating point.
 * 1 sector = 512 bytes on XT hardware (fixed).
 */
static void print_size(unsigned long sectors)
{
    unsigned long bytes = sectors * 512UL;
    if (bytes >= 1024UL * 1024UL)
        printf("    Size:      %lu MB  (%lu sectors)\n",
               bytes / (1024UL * 1024UL), sectors);
    else if (bytes >= 1024UL)
        printf("    Size:      %lu KB  (%lu sectors)\n",
               bytes / 1024UL, sectors);
    else
        printf("    Size:      %lu bytes  (%lu sectors)\n",
               bytes, sectors);
}

/* Unpack and display one 16-byte partition entry (slot is 1-based). */
static void print_entry(int slot, const unsigned char *e)
{
    unsigned char status    = e[0];
    unsigned char part_type = e[4];
    int cyl_s, head_s, sec_s;
    int cyl_e, head_e, sec_e;
    unsigned long lba_start, lba_size;

    /* Check for empty entry */
    if (part_type == 0x00
        && e[1] == 0 && e[2] == 0 && e[3] == 0
        && e[5] == 0 && e[6] == 0 && e[7] == 0) {
        printf("  Partition %d: <empty>\n", slot);
        return;
    }

    unpack_chs(e + 1, &cyl_s, &head_s, &sec_s);
    unpack_chs(e + 5, &cyl_e, &head_e, &sec_e);

    /* LBA fields are 32-bit little-endian */
    lba_start = (unsigned long)e[8]
              | ((unsigned long)e[9]  <<  8)
              | ((unsigned long)e[10] << 16)
              | ((unsigned long)e[11] << 24);
    lba_size  = (unsigned long)e[12]
              | ((unsigned long)e[13] <<  8)
              | ((unsigned long)e[14] << 16)
              | ((unsigned long)e[15] << 24);

    printf("  Partition %d:\n", slot);
    printf("    Status:    %s (0x%02X)\n",
           status == 0x80 ? "Bootable" : "Inactive", status);
    printf("    Type:      0x%02X  (%s)\n", part_type, type_name(part_type));
    if (has_mbr88_sig)
        print_label(slot);
    printf("    Start CHS: C=%d, H=%d, S=%d\n", cyl_s, head_s, sec_s);
    printf("    End   CHS: C=%d, H=%d, S=%d\n", cyl_e, head_e, sec_e);
    printf("    LBA start: %lu\n", lba_start);
    print_size(lba_size);
}

/* Display the full partition table from the global mbr buffer. */
static void print_table(void)
{
    int i;
    printf("\n=== Current Partition Table ===\n");
    for (i = 1; i <= NUM_ENTRIES; i++) {
        int offset = PTABLE_OFFSET + (i - 1) * ENTRY_SIZE;
        print_entry(i, mbr + offset);
    }
    putchar('\n');
}

/* -------------------------------------------------------------------------
 * Input helpers
 * -----------------------------------------------------------------------*/

/*
 * Read a line from stdin into buf (max len bytes including NUL).
 * Strips trailing newline. Returns 0 on EOF, 1 on success.
 */
static int read_line(char *buf, int len)
{
    int n;
    if (fgets(buf, len, stdin) == NULL)
        return 0;
    n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';
    return 1;
}

/* Prompt for an integer in [lo, hi]. Repeats until valid. */
static int ask_int(const char *prompt, int lo, int hi)
{
    char buf[16];
    int val;
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!read_line(buf, sizeof(buf))) {
            fprintf(stderr, "Unexpected EOF.\n");
            exit(1);
        }
        val = atoi(buf);
        if (val >= lo && val <= hi)
            return val;
        printf("  Please enter a value between %d and %d.\n", lo, hi);
    }
}

/* Prompt for a hex or decimal integer in [lo, hi]. Repeats until valid. */
static int ask_hex(const char *prompt, int lo, int hi)
{
    char buf[16];
    int val;
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!read_line(buf, sizeof(buf))) {
            fprintf(stderr, "Unexpected EOF.\n");
            exit(1);
        }
        /* strtol with base 0 accepts 0x prefix or plain decimal */
        val = (int)strtol(buf, NULL, 0);
        if (val >= lo && val <= hi)
            return val;
        printf("  Please enter a value between 0x%02X and 0x%02X.\n", lo, hi);
    }
}

/* Prompt for Y/y or N/n. Returns 1 for yes, 0 for no. */
static int ask_yn(const char *prompt)
{
    char buf[8];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        if (!read_line(buf, sizeof(buf))) {
            fprintf(stderr, "Unexpected EOF.\n");
            exit(1);
        }
        if (buf[0] == 'y' || buf[0] == 'Y') return 1;
        if (buf[0] == 'n' || buf[0] == 'N') return 0;
        printf("  Please press Y or N.\n");
    }
}

/* -------------------------------------------------------------------------
 * Partition entry collection
 * -----------------------------------------------------------------------*/

/*
 * Interactively collect one partition entry and write it into the global
 * mbr buffer at the correct offset for slot (1-based).
 * If has_mbr88_sig, also collect and write the label slot.
 */
static void collect_entry(int slot)
{
    unsigned char entry[ENTRY_SIZE];
    unsigned char label_slot[LABEL_SLOT_SIZE];
    int   bootable, part_type;
    int   cyl_s, head_s, sec_s;
    int   cyl_e, head_e, sec_e;
    unsigned long lba_start, lba_end, lba_size;
    char  label_buf[LABEL_MAX_LEN + 2];
    char  ch_buf[8];
    int   entry_offset, label_offset;

    printf("\n--- Partition %d ---\n", slot);

    /* Bootable */
    bootable = ask_yn("  Bootable? (Y/N): ");

    /* Partition type */
    printf("  Common partition types:\n");
    print_type_hints();
    part_type = ask_hex("  Partition type (hex or decimal, e.g. 0x06 or 6): ",
                        0x00, 0xFF);

    /* Boot menu label — only for mbr88 images */
    if (has_mbr88_sig) {
        for (;;) {
            printf("  Boot menu label (up to %d chars): ", LABEL_MAX_LEN);
            fflush(stdout);
            if (!read_line(label_buf, sizeof(label_buf))) {
                fprintf(stderr, "Unexpected EOF.\n");
                exit(1);
            }
            if ((int)strlen(label_buf) <= LABEL_MAX_LEN)
                break;
            printf("  Label too long — maximum %d characters.\n", LABEL_MAX_LEN);
        }
        /* Ensure at least one space so the menu line isn't bare */
        if (label_buf[0] == '\0') {
            label_buf[0] = ' ';
            label_buf[1] = '\0';
        }
    }

    /* Starting CHS */
    printf("  -- Starting CHS --\n");
    cyl_s  = ask_int("    Cylinder (0-1023): ", 0, 1023);
    head_s = ask_int("    Head     (0-255):  ", 0, 255);
    sec_s  = ask_int("    Sector   (1-63):   ", 1, 63);

    /* Ending CHS */
    printf("  -- Ending CHS --\n");
    cyl_e  = ask_int("    Cylinder (0-1023): ", 0, 1023);
    head_e = ask_int("    Head     (0-255):  ", 0, 255);
    sec_e  = ask_int("    Sector   (1-63):   ", 1, 63);

    /* Compute LBA values from CHS */
    lba_start = chs_to_lba(cyl_s, head_s, sec_s);
    lba_end   = chs_to_lba(cyl_e, head_e, sec_e);
    lba_size  = (lba_end >= lba_start) ? (lba_end - lba_start + 1) : 0;

    /* Print summary */
    printf("\n  Summary:\n");
    printf("    Status:     %s (0x%02X)\n",
           bootable ? "Bootable" : "Inactive",
           bootable ? 0x80 : 0x00);
    printf("    Type:       0x%02X  (%s)\n", part_type, type_name(part_type));
    if (has_mbr88_sig)
        printf("    Label:      '%s'\n", label_buf);
    printf("    Start CHS:  C=%d, H=%d, S=%d  (LBA %lu)\n",
           cyl_s, head_s, sec_s, lba_start);
    printf("    End   CHS:  C=%d, H=%d, S=%d  (LBA %lu)\n",
           cyl_e, head_e, sec_e, lba_end);
    print_size(lba_size);

    /* Build the 16-byte partition entry */
    memset(entry, 0, ENTRY_SIZE);
    entry[0] = (unsigned char)(bootable ? 0x80 : 0x00);
    pack_chs(entry + 1, cyl_s, head_s, sec_s);
    entry[4] = (unsigned char)part_type;
    pack_chs(entry + 5, cyl_e, head_e, sec_e);
    /* LBA start — 32-bit little-endian */
    entry[8]  = (unsigned char)(lba_start & 0xFF);
    entry[9]  = (unsigned char)((lba_start >>  8) & 0xFF);
    entry[10] = (unsigned char)((lba_start >> 16) & 0xFF);
    entry[11] = (unsigned char)((lba_start >> 24) & 0xFF);
    /* LBA size — 32-bit little-endian */
    entry[12] = (unsigned char)(lba_size & 0xFF);
    entry[13] = (unsigned char)((lba_size >>  8) & 0xFF);
    entry[14] = (unsigned char)((lba_size >> 16) & 0xFF);
    entry[15] = (unsigned char)((lba_size >> 24) & 0xFF);

    /* Write entry into global mbr buffer */
    entry_offset = PTABLE_OFFSET + (slot - 1) * ENTRY_SIZE;
    memcpy(mbr + entry_offset, entry, ENTRY_SIZE);

    /* Write label slot into global mbr buffer (mbr88 only) */
    if (has_mbr88_sig) {
        build_label_slot(label_slot, label_buf);
        label_offset = LABEL_BASE_OFFSET + (slot - 1) * LABEL_SLOT_SIZE;
        memcpy(mbr + label_offset, label_slot, LABEL_SLOT_SIZE);
    }

    /* Suppress unused variable warning for ch_buf if not needed */
    (void)ch_buf;
}

/* -------------------------------------------------------------------------
 * File I/O
 * -----------------------------------------------------------------------*/

/* Copy src to dst (for backup). Returns 0 on success, -1 on error. */
static int copy_file(const char *src, const char *dst)
{
    unsigned char buf[512];
    int   fdin, fdout, n;
    int   result = 0;

    fdin = open(src, O_RDONLY);
    if (fdin < 0) {
        perror(src);
        return -1;
    }
    fdout = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fdout < 0) {
        perror(dst);
        close(fdin);
        return -1;
    }
    while ((n = read(fdin, buf, sizeof(buf))) > 0) {
        if (write(fdout, buf, n) != n) {
            perror(dst);
            result = -1;
            break;
        }
    }
    close(fdin);
    close(fdout);
    return result;
}

/* -------------------------------------------------------------------------
 * mbr88 signature detection
 * -----------------------------------------------------------------------*/

static int detect_mbr88(void)
{
    return memcmp(mbr + MBR88_SIG_OFFSET, MBR88_SIG, MBR88_SIG_LEN) == 0;
}

/* -------------------------------------------------------------------------
 * Help text
 * -----------------------------------------------------------------------*/

static void print_help(void)
{
    puts("mbr_patch — Interactive IBM XT-style MBR partition table editor");
    puts("");
    puts("Usage:");
    puts("  mbr_patch <mbr_file>");
    puts("  mbr_patch -h | --help");
    puts("");
    puts("Arguments:");
    puts("  <mbr_file>   Path to a 512-byte MBR binary image to view or edit.");
    puts("               A backup (<mbr_file>.bak) is written before changes.");
    puts("");
    puts("Options:");
    puts("  -h, --help   Show this help text and exit.");
    puts("");
    puts("mbr88 signature detection:");
    puts("  If the 5-byte signature 'mbr88' is found at MBR offset 0x1B9,");
    puts("  boot menu label editing is enabled.  Labels are stored in the");
    puts("  MBR binary at offset 0x43 and displayed by the mbr88 bootloader.");
    puts("  Without the signature only the partition table is edited; the");
    puts("  rest of the MBR binary is left untouched.");
    puts("");
    puts("Partition type codes:");
    puts("  0x00  Empty          0x05  Extended");
    puts("  0x01  FAT12          0x06  FAT16B >=32MB");
    puts("  0x04  FAT16 <32MB    0x0B  FAT32");
    puts("  0x0C  FAT32 LBA      0x80  MINIX old");
    puts("  0x81  MINIX          0x82  Linux swap");
    puts("  0x83  Linux");
    puts("");
    puts("Example:");
    puts("  mbr_patch mbr.bin");
}

/* -------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
    const char *mbr_path;
    char        bak_path[256];
    int         fd, n;
    int         dirty = 0;
    char        choice_buf[8];
    int         choice, i;
    int         active;

    /* Help */
    if (argc == 2 && (strcmp(argv[1], "-h") == 0
                   || strcmp(argv[1], "--help") == 0)) {
        print_help();
        return 0;
    }

    if (argc != 2) {
        fprintf(stderr, "Usage: mbr_patch <mbr_file>\n");
        fprintf(stderr, "       mbr_patch -h | --help\n");
        return 1;
    }

    mbr_path = argv[1];

    /* Read MBR */
    fd = open(mbr_path, O_RDONLY);
    if (fd < 0) {
        perror(mbr_path);
        return 1;
    }
    n = read(fd, mbr, MBR_SIZE);
    close(fd);
    if (n < MBR_SIZE) {
        fprintf(stderr, "Error: '%s' is smaller than 512 bytes.\n", mbr_path);
        return 1;
    }

    /* Boot signature warning */
    if (mbr[BOOTSIG_OFFSET] != 0x55 || mbr[BOOTSIG_OFFSET + 1] != 0xAA)
        printf("Warning: boot signature at 0x1FE is not 55 AA.\n");

    /* mbr88 signature detection */
    has_mbr88_sig = detect_mbr88();
    if (has_mbr88_sig)
        puts("mbr88 boot record detected — boot menu label editing enabled.");
    else {
        puts("Note: 'mbr88' signature not found at offset 0x1B9.");
        puts("      Partition table editing only — label area will not be touched.");
    }

    /* Show current table */
    print_table();

    /* Offer exit before any changes */
    if (!ask_yn("Edit partition table? (Y/N): ")) {
        puts("No changes made.  Exiting.");
        return 0;
    }

    /* Backup */
    if (strlen(mbr_path) > sizeof(bak_path) - 5) {
        fprintf(stderr, "Error: path too long for backup name.\n");
        return 1;
    }
    strcpy(bak_path, mbr_path);
    strcat(bak_path, ".bak");
    if (copy_file(mbr_path, bak_path) != 0) {
        fprintf(stderr, "Error: could not write backup '%s'.\n", bak_path);
        return 1;
    }
    printf("\nBackup written to: %s\n", bak_path);

    /* Drive geometry */
    printf("\n=== IBM XT Drive Geometry ===\n");
    printf("  Default IBM XT: %d heads per cylinder, %d sectors per track.\n",
           DEFAULT_HEADS, DEFAULT_SECTORS);
    if (ask_yn("  Use default XT geometry (16H / 17S)? (Y/N): ")) {
        heads_per_cyl      = DEFAULT_HEADS;
        sectors_per_track  = DEFAULT_SECTORS;
    } else {
        heads_per_cyl     = ask_int("  Heads per cylinder (1-255): ", 1, 255);
        sectors_per_track = ask_int("  Sectors per track  (1-63):  ", 1, 63);
    }
    printf("\n  Geometry: %d heads, %d sectors/track\n",
           heads_per_cyl, sectors_per_track);

    /* Per-slot edit loop */
    for (;;) {
        putchar('\n');
        print_table();
        printf("  Enter partition number to edit (1-4) or Q to quit: ");
        fflush(stdout);
        if (!read_line(choice_buf, sizeof(choice_buf)))
            break;

        if (choice_buf[0] == 'q' || choice_buf[0] == 'Q')
            break;

        choice = atoi(choice_buf);
        if (choice < 1 || choice > 4) {
            puts("  Please enter 1, 2, 3, 4, or Q.");
            continue;
        }

        /* Show current state of this slot */
        putchar('\n');
        print_entry(choice, mbr + PTABLE_OFFSET + (choice - 1) * ENTRY_SIZE);

        /* Collect and write new values into global mbr buffer */
        collect_entry(choice);

        dirty = 1;
        printf("\n  Partition %d updated (not yet written to disk).\n", choice);
    }

    /* Write to disk if anything changed */
    if (dirty) {
        fd = open(mbr_path, O_WRONLY);
        if (fd < 0) {
            perror(mbr_path);
            return 1;
        }
        n = write(fd, mbr, MBR_SIZE);
        close(fd);
        if (n != MBR_SIZE) {
            fprintf(stderr, "Error: short write to '%s'.\n", mbr_path);
            return 1;
        }

        /* Count active partitions */
        active = 0;
        for (i = 0; i < NUM_ENTRIES; i++)
            if (mbr[PTABLE_OFFSET + i * ENTRY_SIZE + 4] != 0)
                active++;

        printf("\nPartition table written to '%s'.\n", mbr_path);
        printf("  %d active entries, %d empty slots.\n",
               active, NUM_ENTRIES - active);
    } else {
        puts("\nNo changes made.");
    }

    return 0;
}
