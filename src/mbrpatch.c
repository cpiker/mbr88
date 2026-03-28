/* mbrpatch.c -- Interactive IBM XT-style MBR partition table editor */

/* MIT License -- Copyright (c) 2026 Chris Piker
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
 */

/* AI Disclosure:
 *   This code was generated with Claude, which is an Artificial Intelligence
 *   service provided by Anthropic. Though design and development was
 *   orchestrated by a human, reviewed by a human and tested by a human,
 *   most of the actual code was composed by an AI.
 *
 *   It is completely reasonable to forbid AI generated software in some
 *   contexts.  Please check the contribution guidelines of any projects you
 *   participate in. If the project has a rule against AI generated software
 *   then DO NOT INCLUDE THIS FILE, in whole or in part, in your patches
 *   or pull requests!
 */

/* Acknowledgements:
 *   Thanks to osdev.org for the reference material and community insights
 *   that informed the design of the mbr88 boot records this tool supports.
 *   https://wiki.osdev.org/MBR_(x86)
 */

/* Build (native Linux / development):
 *   gcc -std=c99 -Wall -o mbrpatch mbrpatch.c
 *
 * Build (cross-compile for ELKS, ia16-elf-gcc):
 *   ia16-elf-gcc -melks -Os -o mbrpatch mbrpatch.c
 *
 * Build (FreeDOS, Open Watcom -- model TBD: tiny or small):
 *   wcl -ms mbrpatch.c -o mbrpatch.exe
 *
 * Disk I/O (-r / -w modes):
 *   Linux / ELKS: the disk device is a path (/dev/sda, /dev/hda, /dev/cda
 *     etc.) opened with open()/read()/write() -- same as any file.
 *     Writing sector 0 requires root permissions on Linux/ELKS.
 *   FreeDOS (Open Watcom): no device-file namespace exists.  The disk is
 *     identified by its BIOS drive number (0x80 = first hard disk, 0x81 =
 *     second, etc.) given as a hex string on the command line (e.g. '80h'
 *     or '0x80').  _bios_disk() from <bios.h> is used for the actual I/O,
 *     requiring no inline assembly.
 *
 * Design notes for ELKS / Open Watcom:
 *   - No malloc/heap: all buffers are static globals or small stack locals.
 *   - No floating point: sizes use integer KB/MB arithmetic.
 *   - Explicit unsigned char/unsigned long for clarity on 16-bit targets.
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

/* -------------------------------------------------------------------------
 * Portability: POSIX (gcc/ia16-elf-gcc) vs Open Watcom
 * -----------------------------------------------------------------------*/
#ifdef __WATCOMC__
#	include <io.h>
#	include <fcntl.h>
#	include <bios.h>
#	define O_RDONLY   _O_RDONLY
#	define O_WRONLY   _O_WRONLY
#	define O_CREAT    _O_CREAT
#	define O_TRUNC    _O_TRUNC
#	define OPEN_MODE  0
#else
#	include <unistd.h>
#	include <fcntl.h>
#	include <sys/stat.h>
#	define OPEN_MODE  0644
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * mbr88 blank template -- included from generated header
 * -----------------------------------------------------------------------*/

#include "mbr88.h"

/* Catch any future mismatch between size constants at compile time */
typedef char mbr_size_check_[
	(MBR88_TEMPLATE_SIZE == 512) ? 1 : -1
];

/* -------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define MBR_SIZE          512
#define PTABLE_OFFSET     0x1BE
#define ENTRY_SIZE        16
#define NUM_ENTRIES       4
#define BOOTSIG_OFFSET    0x1FE

/* MBR88_LABEL_BASE, MBR88_LABEL_SLOT_SZ, MBR88_LABEL_MAX come from mbr88.h */

/* Two-column display geometry -- 38 + 4 + 38 = 80 */
#define COL_WIDTH         38
#define COL_GAP           4
#define LINE_WIDTH        (COL_WIDTH * 2 + COL_GAP)

#define DEFAULT_HEADS     16
#define DEFAULT_SECTORS   17

/* -------------------------------------------------------------------------
 * Session state globals
 * -----------------------------------------------------------------------*/

static unsigned char mbr[MBR_SIZE];
static char          mbr_path[256];
static int           file_exists;
static int           has_mbr88_sig;
static int           dirty;
static int           geo_heads;
static int           geo_sectors;

/* -------------------------------------------------------------------------
 * Partition type table
 * -----------------------------------------------------------------------*/

typedef struct { unsigned char type; const char *name; } TypeName;

static const TypeName TYPE_NAMES[] = {
	{ 0x00, "Empty"            },
	{ 0x01, "FAT12"            },
	{ 0x02, "Xenix root"       },
	{ 0x03, "Xenix usr"        },
	{ 0x04, "FAT16 <32MB"      },
	{ 0x05, "Extended"         },
	{ 0x06, "FAT16B >=32MB"    },
	{ 0x07, "HPFS/NTFS"        },
	{ 0x0A, "OS/2 Boot Mgr"    },
	{ 0x0B, "FAT32"            },
	{ 0x0C, "FAT32 LBA"        },
	{ 0x0E, "FAT16 LBA"        },
	{ 0x0F, "Extended LBA"     },
	{ 0x11, "Hidden FAT12"     },
	{ 0x14, "Hidden FAT16 <32" },
	{ 0x16, "Hidden FAT16B"    },
	{ 0x1B, "Hidden FAT32"     },
	{ 0x1C, "Hidden FAT32 LBA" },
	{ 0x1E, "Hidden FAT16 LBA" },
	{ 0x64, "NetWare 286"      },
	{ 0x80, "MINIX orig/ELKS"  },
	{ 0x81, "MINIX"            },
	{ 0x82, "Linux swap"       },
	{ 0x83, "Linux"            },
	{ 0xDB, "CP/M-86"          },
	{ 0x00, NULL               }
};

static const char *type_name(unsigned char t)
{
	int i;
	for (i = 0; TYPE_NAMES[i].name; i++)
		if (TYPE_NAMES[i].type == t)
			return TYPE_NAMES[i].name;
	return "Unknown";
}

/* Print partition type list in two columns, no 0x prefix -- the 't' command
 * calls this.  Any value 00-FF is accepted by the type prompt; entries not
 * in this table display as "Unknown" in the partition summary. */
static void print_type_hints(void)
{
	int i, count;

	/* Count entries (excluding sentinel) */
	for (count = 0; TYPE_NAMES[count].name; count++)
		;

	/* Two columns: left column takes ceil(count/2) entries */
	{
		int half = (count + 1) / 2;
		for (i = 0; i < half; i++) {
			int r = i + half;  /* right column index */
			if (r < count)
				printf("    %02X  %-18s    %02X  %s\n",
					TYPE_NAMES[i].type, TYPE_NAMES[i].name,
					TYPE_NAMES[r].type, TYPE_NAMES[r].name);
			else
				printf("    %02X  %s\n",
					TYPE_NAMES[i].type, TYPE_NAMES[i].name);
		}
	}
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

static void build_label_slot(unsigned char slot[MBR88_LABEL_SLOT_SZ],
	const char *text)
{
	int len = (int)strlen(text);
	if (len > MBR88_LABEL_MAX) len = MBR88_LABEL_MAX;
	memset(slot, 0, MBR88_LABEL_SLOT_SZ);
	memcpy(slot, text, len);
	slot[len]   = '\r';
	slot[len+1] = '\n';
	slot[len+2] = '\0';
}

static void read_label(int slot_0based, char buf[MBR88_LABEL_MAX + 1])
{
	int off = MBR88_LABEL_BASE + slot_0based * MBR88_LABEL_SLOT_SZ;
	int len;
	memcpy(buf, mbr + off, MBR88_LABEL_MAX);
	buf[MBR88_LABEL_MAX] = '\0';
	len = (int)strlen(buf);
	while (len > 0 && (buf[len-1] == ' '  || buf[len-1] == '\t' ||
		buf[len-1] == '\r' || buf[len-1] == '\n'))
		buf[--len] = '\0';
}

static void write_label(int slot_0based, const char *text)
{
	unsigned char slot[MBR88_LABEL_SLOT_SZ];
	build_label_slot(slot, text);
	memcpy(
		mbr + MBR88_LABEL_BASE + slot_0based * MBR88_LABEL_SLOT_SZ,
		slot,
		MBR88_LABEL_SLOT_SZ
	);
}

/* -------------------------------------------------------------------------
 * mbr88 detection and upgrade
 * -----------------------------------------------------------------------*/

static int detect_mbr88(void)
{
	return memcmp(mbr + MBR88_SIG_OFFSET,
		MBR88_TEMPLATE + MBR88_SIG_OFFSET,
		MBR88_SIG_LEN) == 0;
}

/* Read version byte from MBR88_VER_OFFSET.
 * Returns major in high nibble, minor in low nibble.
 * Caller formats as "vX.Y". */
static unsigned char mbr88_version(void)
{
	return mbr[MBR88_VER_OFFSET];
}

/* Return 1 if label editing is supported for the loaded MBR.
 * Label slot format is only understood for mbr88 v0.1 exactly.
 * Any other version (including future ones) is unsupported; the
 * caller should direct the user to get a newer mbrpatch. */
static int labels_supported(void)
{
	return detect_mbr88() && (mbr88_version() == 0x01);
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
	fdout = open(dst, O_WRONLY | O_CREAT | O_TRUNC, OPEN_MODE);
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

/* ask_hex: prompt for a hex or decimal value in [lo,hi].
 * base=16  forces hex interpretation (no prefix needed).
 * base=0   auto-detects: 0x prefix = hex, trailing h = hex, else decimal.
 * Use base=16 for partition types; use base=0 for BIOS drive numbers. */
static int ask_hex(const char *prompt, int lo, int hi, int base)
{
	char buf[16];
	int val;
	for (;;) {
		printf("%s", prompt); fflush(stdout);
		if (!read_line(buf, sizeof(buf))) { fprintf(stderr,"EOF\n"); exit(1); }
		val = (int)strtol(buf, NULL, base);
		if (val >= lo && val <= hi) return val;
		if (base == 16)
			printf("  Please enter a hex value between %02X and %02X.\n", lo, hi);
		else
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

static void col_row(char line[COL_WIDTH+1], const char *s)
{
	snprintf(line, COL_WIDTH+1, "%-*.*s", COL_WIDTH, COL_WIDTH, s);
}

static void col_lines(int slot_1based, char lines[7][COL_WIDTH + 1])
{
	int i = slot_1based - 1;
	int n;
	char tmp[64];

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
		char label_buf[MBR88_LABEL_MAX + 1];
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
	const char *sig_tag;
	char geo_buf[32];
	char ver_buf[16];
	int n, len, pad, rpad;

	memset(gap, ' ', COL_GAP);
	gap[COL_GAP] = '\0';

	for (n = 0; n < LINE_WIDTH; n++) putchar('=');
	putchar('\n');

	if (dirty)
		snprintf(title, sizeof(title), "MBR Partition Table  (* unsaved changes)");
	else
		snprintf(title, sizeof(title), "MBR Partition Table");
	len  = (int)strlen(title);
	pad  = (LINE_WIDTH - len) / 2;
	rpad = LINE_WIDTH - len - pad;
	printf("%*s%s%*s\n", pad, "", title, rpad, "");

	for (n = 0; n < LINE_WIDTH; n++) putchar('=');
	putchar('\n');

	col_lines(1, left);
	col_lines(2, right);
	for (n = 0; n < 7; n++)
		printf("%s%s%s\n", left[n], gap, right[n]);

	for (n = 0; n < LINE_WIDTH; n++) putchar('-');
	putchar('\n');

	col_lines(3, left);
	col_lines(4, right);
	for (n = 0; n < 7; n++)
		printf("%s%s%s\n", left[n], gap, right[n]);

	for (n = 0; n < LINE_WIDTH; n++) putchar('=');
	putchar('\n');

	/* Status bar */
	if (has_mbr88_sig) {
		unsigned char ver = mbr88_version();
		snprintf(ver_buf, sizeof(ver_buf), "[mbr88 v%d.%d]",
			(ver >> 4) & 0x0F, ver & 0x0F);
		sig_tag = ver_buf;
	} else {
		sig_tag = "[generic MBR]";
	}

	if (geo_heads)
		snprintf(geo_buf, sizeof(geo_buf), "Geometry: %dH/%dS",
			geo_heads, geo_sectors);
	else
		geo_buf[0] = '\0';   /* blank until set -- use 'g' to configure */

	{
		const char *base = strrchr(mbr_path, '/');
		base = base ? base + 1 : mbr_path;
		if (geo_buf[0])
			printf("  File: %s  %s  %s\n\n", base, sig_tag, geo_buf);
		else
			printf("  File: %s  %s  Geometry: (use 'g' to set)\n\n",
				base, sig_tag);
	}
}

/* -------------------------------------------------------------------------
 * Command implementations
 * -----------------------------------------------------------------------*/

static void cmd_geometry(void)
{
	printf("\n");
	printf("Drive geometry converts CHS addresses to LBA sector numbers.\n\n");
	printf("IBM XT default: %d heads per cylinder, %d sectors per track.\n",
		DEFAULT_HEADS, DEFAULT_SECTORS);
	printf("Check your drive spec or use values reported by fdisk.\n\n");

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
		printf("  Geometry not set -- run 'g' first.\n");
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
	part_type = ask_hex("  Partition type (hex, no prefix): ", 0x00, 0xFF, 16);
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

	if (!ask_yn("  Delete partition? Zeros all 16 bytes of the entry "
		"and its label. (Y/N): "))
		return;

	off = PTABLE_OFFSET + (slot - 1) * ENTRY_SIZE;
	memset(mbr + off, 0, ENTRY_SIZE);

	if (has_mbr88_sig)
		memset(
			mbr + MBR88_LABEL_BASE + (slot-1) * MBR88_LABEL_SLOT_SZ,
			0,
			MBR88_LABEL_SLOT_SZ
		);

	dirty = 1;
	printf("  Partition %d deleted.\n", slot);
}

static void cmd_set_type(void)
{
	int slot, part_type;

	slot = ask_slot("  Partition number (1-4): ");
	if (slot < 0) return;

	if (is_empty(slot - 1)) {
		printf("  Partition %d is empty -- use 'n' to define it first.\n", slot);
		return;
	}

	printf("\n  Common partition types:\n");
	print_type_hints();
	part_type = ask_hex("  New type (hex, no prefix): ", 0x00, 0xFF, 16);

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
		printf("  Partition %d is empty -- use 'n' to define it first.\n", slot);
		return;
	}

	off     = PTABLE_OFFSET + (slot-1) * ENTRY_SIZE;
	current = mbr[off];
	new_val = (current == 0x80) ? 0x00 : 0x80;
	mbr[off] = new_val;
	dirty = 1;
	printf("  Partition %d is now %s (0x%02X).\n", slot,
		new_val == 0x80 ? "bootable" : "inactive", new_val);
	printf("  Note: mbr88 shows all bootable partitions; other loaders may not.\n");
}

static void cmd_label(void)
{
	int  slot;
	char label_buf[MBR88_LABEL_MAX + 2];
	char current[MBR88_LABEL_MAX + 1];

	if (!has_mbr88_sig) {
		printf("  Labels are only supported on mbr88 images.\n");
		printf("  Use -u to upgrade this MBR to mbr88 and enable labels.\n");
		return;
	}

	slot = ask_slot("  Partition number (1-4): ");
	if (slot < 0) return;

	if (is_empty(slot - 1)) {
		printf("  Partition %d is empty -- use 'n' to define it first.\n", slot);
		return;
	}

	read_label(slot - 1, current);
	printf("  Current label: '%s'\n", current);

	for (;;) {
		printf("  New label (up to %d chars): ", MBR88_LABEL_MAX);
		fflush(stdout);
		if (!read_line(label_buf, sizeof(label_buf))) exit(1);
		if ((int)strlen(label_buf) <= MBR88_LABEL_MAX) break;
		printf("  Label too long -- maximum %d characters.\n", MBR88_LABEL_MAX);
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
	printf("\n  Common partition type codes (enter hex without prefix):\n");
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

	/* No full redraw here -- the table was already shown after the last
	 * modifying command.  Just confirm and write. */
	active = 0;
	for (i = 0; i < NUM_ENTRIES; i++)
		if (mbr[PTABLE_OFFSET + i * ENTRY_SIZE + 4] != 0)
			active++;

	printf("  %d active partition(s).  Write changes to '%s'? (Y/N): ",
		active, mbr_path);
	fflush(stdout);
	{
		char buf[8];
		if (!read_line(buf, sizeof(buf))) return;
		if (buf[0] != 'y' && buf[0] != 'Y') {
			printf("  Write cancelled.\n");
			return;
		}
	}

	if (file_exists) {
		snprintf(bak, sizeof(bak), "%s.bak", mbr_path);
		if (copy_file(mbr_path, bak) == 0)
			printf("  Backup written to: %s\n", bak);
		else {
			fprintf(stderr, "Error: could not write backup.\n");
			return;
		}
	}

	fd = open(mbr_path, O_WRONLY | O_CREAT | O_TRUNC, OPEN_MODE);
	if (fd < 0) { perror(mbr_path); return; }
	n = write(fd, mbr, MBR_SIZE);
	close(fd);
	if (n != MBR_SIZE) {
		fprintf(stderr, "Error: short write.\n");
		return;
	}

	printf("  Written to '%s'.  %d active, %d empty.\n",
		mbr_path, active, NUM_ENTRIES - active);
	dirty       = 0;
	file_exists = 1;
}

static void cmd_help(void)
{
	printf("\n  Commands:\n");
	printf("    g  Set drive geometry (required before 'n')\n");
	printf("    n  Define a new partition (or redefine an existing one)\n");
	printf("    d  Delete a partition slot (zeros all 16 bytes and label)\n");
	printf("    t  Set the partition type byte\n");
	printf("    b  Toggle the bootable flag (inactive / bootable)\n");
	printf("    v  Set the boot menu volume label  (mbr88 images only)\n");
	printf("    p  Print the partition table\n");
	printf("    l  List common partition type codes\n");
	printf("    w  Write changes to file\n");
	printf("    h  This help text\n");
	printf("    q  Quit (prompts if there are unsaved changes)\n");
	putchar('\n');
}

/* -------------------------------------------------------------------------
 * Help text -- full version for native builds, minimal for target builds
 * -----------------------------------------------------------------------*/

#if defined(__ia16__) || defined(__WATCOMC__)

static void print_help(void)
{
	puts("mbrpatch <file>                   view MBR image file");
	puts("mbrpatch -p <file>                patch MBR image file");
	puts("mbrpatch -u <file>                upgrade MBR image to mbr88");
	puts("mbrpatch -n <file>                create new blank mbr88 image");
#ifdef __WATCOMC__
	puts("mbrpatch -r <file> <drive>        read disk MBR to file");
	puts("  drive: 80h=hard disk 1, 81h=hard disk 2, etc.");
	puts("mbrpatch -w <file> <drive>        write file to disk MBR");
#else
	puts("mbrpatch -r <file> <device>       read disk MBR to file");
	puts("mbrpatch -w <file> <device>       write file to disk MBR");
#endif
	puts("Use -h on a native system for full help.");
}

#else

static void print_help(void)
{
	puts("mbrpatch -- IBM XT-style MBR partition table viewer and editor");
	puts("");
	puts("Usage:");
	puts("  mbrpatch        <mbr_file>               View partition table and exit");
	puts("  mbrpatch -p     <mbr_file>               Patch an existing MBR");
	puts("  mbrpatch -u     <mbr_file>               Upgrade existing MBR to mbr88");
	puts("  mbrpatch -n     <mbr_file>               Create a new blank mbr88 image");
	puts("  mbrpatch -r     <mbr_file> <device>      Read MBR from disk to file");
	puts("  mbrpatch -w     <mbr_file> <device>      Write MBR from file to disk");
	puts("  mbrpatch -h | --help                     Show this help text and exit");
	puts("");
	puts("Modes:");
	puts("  (no flag)  Read-only view.  Prints the partition table and exits.");
	puts("             The file must exist, be exactly 512 bytes, and have a");
	puts("             valid 55 AA boot signature.  Exits non-zero on error.");
	puts("");
	puts("  -p         Patch.  Interactively edit the partition table of an");
	puts("             existing MBR file.  Label editing is enabled only for");
	puts("             mbr88 v0.1 images.  Other mbr88 versions show a message");
	puts("             directing you to get a newer mbrpatch.");
	puts("");
	puts("  -u         Upgrade.  Replace the boot code of an existing MBR with");
	puts("             the mbr88 v0.1 boot record, preserving the partition");
	puts("             table entries.  The file must exist and be valid.  Label");
	puts("             editing is always available after upgrade.");
	puts("");
	puts("  -n         New.  Create a blank mbr88 v0.1 image from scratch.");
	puts("             The target file must NOT exist (safety check).  Enters");
	puts("             the same interactive session as -p with labels enabled.");
	puts("");
	puts("  -r         Read disk MBR.  Reads the first sector of <device>,");
	puts("             displays the partition table, and saves it to <mbr_file>.");
	puts("             The output file must not already exist (safety check).");
	puts("             On Linux/ELKS: device is a path, e.g. /dev/sda, /dev/hda.");
	puts("             On FreeDOS: device is the BIOS drive number, e.g. 80h.");
	puts("");
	puts("  -w         Write disk MBR.  Loads <mbr_file>, displays the partition");
	puts("             table, asks for confirmation, then writes the 512-byte");
	puts("             image to the first sector of <device>.  Run -r first to");
	puts("             save a backup before using -w.");
	puts("");
	puts("A backup (<mbr_file>.bak) is written before any file write (-p/-u/-n).");
	puts("There is no automatic backup for device writes (-w); use -r first.");
	puts("");
	puts("Interactive commands (type 'h' at the prompt for a summary):");
	puts("  g  Set drive geometry       n  New / redefine partition");
	puts("  d  Delete partition         t  Set partition type");
	puts("  b  Toggle bootable flag     v  Set boot menu volume label");
	puts("  p  Print table              l  List type codes");
	puts("  w  Write to file            q  Quit");
	puts("");
	puts("mbr88 bootable flag:");
	puts("  mbr88 only shows bootable partitions in the boot menu.");
	puts("  Unlike most MBR loaders, mbr88 allows multiple partitions to be");
	puts("  bootable at once -- all will appear in the menu.");
	puts("  Use the 'b' command to toggle the flag on each partition.");
	puts("");
	puts("mbr88 label editing:");
	puts("  Labels are stored in the MBR at offset 0x41 and displayed in the");
	puts("  boot menu next to the partition number.  They are only supported");
	puts("  for mbr88 v0.1 images.  If a newer mbr88 version is detected,");
	puts("  get an updated mbrpatch from: https://github.com/cpiker/mbr88");
	puts("");
	puts("Examples:");
	puts("  mbrpatch mbr.bin                    view current partition table");
	puts("  mbrpatch -r backup.bin /dev/sda     read live MBR to file (Linux)");
	puts("  mbrpatch -r backup.bin 80h          read live MBR to file (FreeDOS)");
	puts("  mbrpatch -u mbr.bin                 upgrade to mbr88, then edit");
	puts("  mbrpatch -w mbr.bin /dev/sda        write MBR to disk (Linux)");
	puts("  mbrpatch -w mbr.bin 80h             write MBR to disk (FreeDOS)");
	puts("  mbrpatch -n new.bin                 create a fresh mbr88 image");
}

#endif /* __ia16__ || __WATCOMC__ */

/* -------------------------------------------------------------------------
 * Disk I/O -- read or write the first sector of a physical disk.
 *
 * On Linux / ELKS the caller passes a device path string (/dev/sda etc.).
 * On FreeDOS (Open Watcom) the caller passes a BIOS drive number string
 * such as "80h", "0x80", or "128" -- all meaning the first hard disk.
 * -----------------------------------------------------------------------*/

#ifdef __WATCOMC__

/* Parse a BIOS drive number from a string.
 * Accepts: "80h" / "80H" (hex without 0x prefix, trailing h),
 *          "0x80" / "0X80" (C-style hex), plain decimal "128".
 * Returns the drive number, or -1 if the string is not parseable. */
static int disk_parse_drive(const char *s)
{
	char buf[8];
	int  i, len;
	long val;
	char *end;

	len = (int)strlen(s);
	if (len == 0 || len > 6) return -1;

	/* Copy to mutable buffer, strip trailing h/H */
	for (i = 0; i < len && i < (int)sizeof(buf)-1; i++)
		buf[i] = s[i];
	buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf)-1] = '\0';
	if (buf[len-1] == 'h' || buf[len-1] == 'H') {
		buf[len-1] = '\0';
		val = strtol(buf, &end, 16);
	} else {
		val = strtol(buf, &end, 0);  /* handles 0x prefix or plain decimal */
	}
	if (end == buf || *end != '\0') return -1;
	if (val < 0x80 || val > 0xFF) return -1;
	return (int)val;
}

static int disk_read_mbr(const char *device, unsigned char *buf)
{
	struct diskinfo_t di;
	unsigned int      rc;
	int               drive;

	drive = disk_parse_drive(device);
	if (drive < 0) {
		fprintf(stderr,
			"Error: '%s' is not a valid drive number.\n"
			"  Use 80h for the first hard disk, 81h for the second, etc.\n",
			device);
		return 1;
	}
	di.drive    = (unsigned)drive;
	di.head     = 0;
	di.track    = 0;
	di.sector   = 1;          /* BIOS sectors are 1-based */
	di.nsectors = 1;
	di.buffer   = buf;
	rc = _bios_disk(_DISK_READ, &di);
	if (rc & 0xFF00) {
		fprintf(stderr, "Error: INT 13h read failed (status 0x%02X).\n",
			(rc >> 8) & 0xFF);
		return 1;
	}
	return 0;
}

static int disk_write_mbr(const char *device, const unsigned char *buf)
{
	struct diskinfo_t di;
	unsigned int      rc;
	int               drive;

	drive = disk_parse_drive(device);
	if (drive < 0) {
		fprintf(stderr,
			"Error: '%s' is not a valid drive number.\n"
			"  Use 80h for the first hard disk, 81h for the second, etc.\n",
			device);
		return 1;
	}
	di.drive    = (unsigned)drive;
	di.head     = 0;
	di.track    = 0;
	di.sector   = 1;
	di.nsectors = 1;
	di.buffer   = (void *)buf;   /* Watcom _bios_disk takes non-const void * */
	rc = _bios_disk(_DISK_WRITE, &di);
	if (rc & 0xFF00) {
		fprintf(stderr, "Error: INT 13h write failed (status 0x%02X).\n",
			(rc >> 8) & 0xFF);
		return 1;
	}
	return 0;
}

#else  /* Linux / ELKS: device is a filesystem path */

static int disk_read_mbr(const char *device, unsigned char *buf)
{
	int fd, n;
	fd = open(device, O_RDONLY);
	if (fd < 0) { perror(device); return 1; }
	n = read(fd, buf, MBR_SIZE);
	close(fd);
	if (n != MBR_SIZE) {
		fprintf(stderr,
			"Error: read from '%s' returned %d bytes (expected %d).\n",
			device, n, MBR_SIZE);
		return 1;
	}
	return 0;
}

static int disk_write_mbr(const char *device, const unsigned char *buf)
{
	int fd, n;
	fd = open(device, O_WRONLY);
	if (fd < 0) { perror(device); return 1; }
	n = write(fd, buf, MBR_SIZE);
	close(fd);
	if (n != MBR_SIZE) {
		fprintf(stderr,
			"Error: write to '%s' returned %d bytes (expected %d).\n",
			device, n, MBR_SIZE);
		return 1;
	}
	return 0;
}

#endif /* __WATCOMC__ */

/* Read mbr_path into the global mbr buffer.  Checks:
 *   - file exists and can be opened
 *   - exactly 512 bytes readable
 *   - boot signature 55 AA at 0x1FE
 * Prints an error and returns non-zero on any failure. */
static int load_and_validate(void)
{
	int fd, n;

	fd = open(mbr_path, O_RDONLY);
	if (fd < 0) {
		perror(mbr_path);
		return 1;
	}
	n = read(fd, mbr, MBR_SIZE);
	close(fd);

	if (n != MBR_SIZE) {
		fprintf(stderr, "Error: '%s' is not 512 bytes.\n", mbr_path);
		return 1;
	}
	if (mbr[BOOTSIG_OFFSET] != 0x55 || mbr[BOOTSIG_OFFSET+1] != 0xAA) {
		fprintf(stderr,
			"Error: '%s' has no valid MBR signature (55 AA) at 0x1FE.\n",
			mbr_path);
		return 1;
	}
	file_exists = 1;
	return 0;
}

/* -------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
	/* mode flags -- exactly one will be set */
	int mode_view    = 0;
	int mode_patch   = 0;
	int mode_upgrade = 0;
	int mode_new     = 0;
	int mode_read    = 0;
	int mode_write   = 0;

	char disk_device[256];   /* -r / -w: device path or BIOS drive number */
	int  fd;
	char cmd_buf[8];

	disk_device[0] = '\0';

	/* -h / --help */
	if (argc == 2 &&
		(strcmp(argv[1],"-h")==0 || strcmp(argv[1],"--help")==0)) {
		print_help();
		return 0;
	}

	/* Parse mode flag.  All options are single characters after '-' so we
	 * switch on argv[1][1].  The -r and -w cases take two arguments
	 * (file then device); all other flagged modes take one (file). */
	if (argc >= 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
		switch (argv[1][1]) {
		case 'r':
		case 'w':
			if (argc != 4) {
				fprintf(stderr,
					"Usage: mbrpatch %s <mbr_file> <device>\n",
					argv[1]);
				return 1;
			}
			if (argv[1][1] == 'r') mode_read  = 1;
			else                    mode_write = 1;
			strncpy(mbr_path,    argv[2], sizeof(mbr_path)    - 1);
			strncpy(disk_device, argv[3], sizeof(disk_device) - 1);
			mbr_path[sizeof(mbr_path)-1]       = '\0';
			disk_device[sizeof(disk_device)-1] = '\0';
			break;
		case 'p':
		case 'u':
		case 'n':
			if (argc != 3) {
				fprintf(stderr, "Usage: mbrpatch %s <mbr_file>\n", argv[1]);
				return 1;
			}
			switch (argv[1][1]) {
			case 'p': mode_patch   = 1; break;
			case 'u': mode_upgrade = 1; break;
			case 'n': mode_new     = 1; break;
			}
			strncpy(mbr_path, argv[2], sizeof(mbr_path) - 1);
			mbr_path[sizeof(mbr_path)-1] = '\0';
			break;
		default:
			fprintf(stderr, "Unknown option '%s'.\n", argv[1]);
			fprintf(stderr,
				"Usage: mbrpatch [-p|-u|-n] <mbr_file>\n"
				"       mbrpatch -r|-w <mbr_file> <device>\n"
				"       mbrpatch -h\n");
			return 1;
		}
	} else if (argc == 2) {
		mode_view = 1;
		strncpy(mbr_path, argv[1], sizeof(mbr_path) - 1);
		mbr_path[sizeof(mbr_path)-1] = '\0';
	} else {
		fprintf(stderr,
			"Usage: mbrpatch [-p|-u|-n] <mbr_file>\n"
			"       mbrpatch -r|-w <mbr_file> <device>\n"
			"       mbrpatch -h\n");
		return 1;
	}

	/* ------------------------------------------------------------------ */
	/* Mode -r: read first sector from disk, validate, display, save      */
	/* ------------------------------------------------------------------ */
	if (mode_read) {
		int fd2, n2;

		printf("Reading MBR from: %s\n", disk_device);
		if (disk_read_mbr(disk_device, mbr) != 0)
			return 1;

		if (mbr[BOOTSIG_OFFSET] != 0x55 || mbr[BOOTSIG_OFFSET+1] != 0xAA) {
			fprintf(stderr,
				"Warning: sector does not have a valid 55 AA boot signature.\n"
				"  The sector will be saved as-is.\n");
		}

		has_mbr88_sig = detect_mbr88();
		print_table();

		/* Save to file -- must not already exist (safety) */
		fd2 = open(mbr_path, O_RDONLY);
		if (fd2 >= 0) {
			close(fd2);
			fprintf(stderr,
				"Error: '%s' already exists.  Remove it first or choose a\n"
				"  different filename to avoid overwriting an existing backup.\n",
				mbr_path);
			return 1;
		}
		fd2 = open(mbr_path, O_WRONLY | O_CREAT | O_TRUNC, OPEN_MODE);
		if (fd2 < 0) { perror(mbr_path); return 1; }
		n2 = write(fd2, mbr, MBR_SIZE);
		close(fd2);
		if (n2 != MBR_SIZE) {
			fprintf(stderr, "Error: short write to '%s'.\n", mbr_path);
			return 1;
		}
		printf("  Saved to '%s'.\n", mbr_path);
		return 0;
	}

	/* ------------------------------------------------------------------ */
	/* Mode -w: load file, display it, confirm, write to disk             */
	/* ------------------------------------------------------------------ */
	if (mode_write) {
		if (load_and_validate() != 0)
			return 1;

		has_mbr88_sig = detect_mbr88();
		print_table();

		printf("  Target device : %s\n", disk_device);
		printf("  This will OVERWRITE the first sector of that device.\n");
		printf("  There is no automatic backup for a device write.\n");
		printf("  Make sure you have saved a copy of the existing MBR\n");
		printf("  (use mbrpatch -r) before proceeding.\n\n");
		if (!ask_yn("Write MBR to device? (Y/N): ")) {
			printf("  Write cancelled.\n");
			return 0;
		}

		if (disk_write_mbr(disk_device, mbr) != 0)
			return 1;

#if !defined(__WATCOMC__)
		sync();
#endif
		printf("  Written to %s.\n", disk_device);
		return 0;
	}

	/* ------------------------------------------------------------------ */
	/* Mode: view -- read, validate, print, exit                          */
	/* ------------------------------------------------------------------ */
	if (mode_view) {
		if (load_and_validate() != 0)
			return 1;

		if (detect_mbr88()) {
			unsigned char ver = mbr88_version();
			has_mbr88_sig = labels_supported();
			printf("mbr88 v%d.%d\n",
				(ver >> 4) & 0x0F, ver & 0x0F);
		}
		print_table();
		return 0;
	}

	/* ------------------------------------------------------------------ */
	/* Mode: new -- file must NOT exist                                    */
	/* ------------------------------------------------------------------ */
	if (mode_new) {
		fd = open(mbr_path, O_RDONLY);
		if (fd >= 0) {
			close(fd);
			fprintf(stderr,
				"Error: '%s' already exists.  Use -u to upgrade an "
				"existing MBR,\n"
				"       or remove the file first.\n", mbr_path);
			return 1;
		}
		memcpy(mbr, MBR88_TEMPLATE, MBR_SIZE);
		has_mbr88_sig = 1;
		file_exists   = 0;
		puts("New mbr88 v0.1 image (blank partition table).");
		puts("  Use 'g' then 'n' to define partitions, 'v' for labels, 'w' to write.");
		print_table();
		/* fall through to command loop */
	}

	/* ------------------------------------------------------------------ */
	/* Modes: patch / upgrade -- file must exist and be valid             */
	/* ------------------------------------------------------------------ */
	if (mode_patch || mode_upgrade) {
		if (load_and_validate() != 0)
			return 1;

		if (mode_upgrade) {
			upgrade_to_mbr88();
			has_mbr88_sig = 1;
			dirty = 1;   /* boot code was replaced -- treat as unsaved change */
			puts("Upgrade: boot code replaced with mbr88 v0.1, "
				"partition table preserved.");
			puts("  Use 'v' to set labels, 'w' to write when done.");
		} else {
			/* mode_patch: detect signature and version */
			if (detect_mbr88()) {
				unsigned char ver = mbr88_version();
				if (labels_supported()) {
					has_mbr88_sig = 1;
					printf("mbr88 v%d.%d -- label editing enabled.\n",
						(ver >> 4) & 0x0F, ver & 0x0F);
				} else {
					has_mbr88_sig = 0;
					printf("mbr88 v%d.%d detected -- label editing not "
						"supported by this version of mbrpatch.\n",
						(ver >> 4) & 0x0F, ver & 0x0F);
					puts("  Get a newer mbrpatch: "
						"https://github.com/cpiker/mbr88");
				}
			} else {
				has_mbr88_sig = 0;
				puts("Generic MBR -- partition table editing only.");
				puts("  Use -u to upgrade to mbr88 and enable label editing.");
			}
		}
		print_table();
	}

	/* ------------------------------------------------------------------ */
	/* Command loop -- shared by -p, -u, -n                               */
	/* ------------------------------------------------------------------ */
	for (;;) {
		int redraw = 0;
		printf("Command (h for help): "); fflush(stdout);
		if (!read_line(cmd_buf, sizeof(cmd_buf))) break;
		if (!cmd_buf[0]) continue;

		switch (cmd_buf[0]) {
		case 'q': case 'Q':
			if (dirty &&
				!ask_yn("Unsaved changes -- quit without writing? (Y/N): "))
				continue;
			return 0;
		case 'g': case 'G':  cmd_geometry(); redraw=1; break;
		case 'n': case 'N':  cmd_new();      redraw=1; break;
		case 'd': case 'D':  cmd_delete();   redraw=1; break;
		case 't': case 'T':  cmd_set_type(); redraw=1; break;
		case 'b': case 'B':  cmd_bootable(); redraw=1; break;
		case 'v': case 'V':  cmd_label();    redraw=1; break;
		case 'p': case 'P':  print_table();            break;
		case 'l': case 'L':  cmd_types();              break;
		case 'w': case 'W':  cmd_write();    redraw=1; break;
		case 'h': case 'H':  cmd_help();               break;
		default:
			printf("  Unknown command '%c'. Type 'h' for help.\n",
				cmd_buf[0]);
			break;
		}

		if (redraw) print_table();
	}

	return 0;
}
