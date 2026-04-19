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
 *   This code was generated in cooperation with Claude, which is an
 *   Artificial Intelligence service provided by Anthropic. Though
 *   development was orchestrated by a human, reviewed by a human, and
 *   tested by a human, the majority of the code was composed by an AI.
 *
 *   It is completely reasonable to forbid AI generated software in some
 *   contexts.  Please check the contribution guidelines of any projects
 *   you participate in. If the project has a rule against AI generated
 *   software then DO NOT INCLUDE THIS FILE, in whole or in part, in your
 *   patches or pull requests!
 */

/* Acknowledgements:
 *   Thanks to osdev.org for the reference material and community insights
 *   that informed the design of the MBR88 boot records this tool provides.
 *   https://wiki.osdev.org/MBR_(x86)
 */

/* Build (native Linux / development):
 *   gcc -std=c99 -Wall -o mbrpatch mbrpatch.c
 *
 * Build (cross-compile for ELKS, ia16-elf-gcc):
 *   ia16-elf-gcc -melks -Os -o mbrpatch mbrpatch.c
 *
 * Build (FreeDOS, Open Watcom 2 -- tiny model .com):
 *   wcl -bt=dos -mt -0 -os -zq -fo=build/freedos/ \
 *       -fe=build/freedos/mbrpatch.com -Ibuild/freedos src/mbrpatch.c
 *
 * Disk I/O (-r / -w modes):
 *   Linux / ELKS: the disk device is a path (/dev/hda, /dev/cda
 *     etc.) opened with open()/read()/write() -- same as any file.
 *     Writing sector 0 requires root permissions on Linux/ELKS.
 *   FreeDOS (Open Watcom): no device-file namespace exists.  The disk is
 *     identified by an ordinal drive number (1 = first hard disk, 2 = second)
 *     on the command line.  The legacy BIOS hex forms '80h' / '0x80' are also
 *     accepted for backward compatibility.  _bios_disk() from <bios.h> is
 *     used for the actual I/O, requiring no inline assembly.
 *
 * Command-line prefix convention:
 *   Linux / ELKS: '-' prefix only (e.g. -r, -w, -h).
 *   FreeDOS:      '/' prefix preferred (e.g. /R, /W, /?), following DOS
 *                 convention; '-' prefix also accepted for familiarity.
 *                 '/?' is the primary help trigger; '/H' is a silent alias.
 *
 * Switch letter case:
 *   All switch comparisons are case-insensitive on both platforms.
 *   DOS canonical form uses uppercase (/R, /W, /P, /U, /N, /H, /?).
 *   Linux canonical form uses lowercase (-r, -w, -p, -u, -n, -h).
 *   The non-canonical case works silently but is not shown in help text.
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

/* FILE SECTION INDEX -- grep for the tag to jump to that section
 *
 *   SEC:PORTABILITY      #ifdef portability block and includes
 *   SEC:TEMPLATE         mbr88 template include and size check
 *   SEC:CONSTANTS        #define constants
 *   SEC:GLOBALS          Session-state global variables
 *   SEC:TYPETABLE        Partition type name table and lookup
 *   SEC:CHS              CHS pack/unpack/to-LBA helpers
 *   SEC:LABELS           Label slot build/read/write helpers
 *   SEC:MBR88DETECT      mbr88 signature detection and upgrade
 *   SEC:HOSTILEDETECT    Hostile MBR detection (GPT block, GRUB/LILO warn)
 *   SEC:FILEIO           copy_file, load_and_validate
 *   SEC:INPUT            read_line, ask_int, ask_hex, ask_yn, read_diskid,
 *                        ask_slot
 *   SEC:DISPLAY          Two-column table display, print_table_gpt,
 *                        print_table
 *   SEC:DISKID           diskid_is_zero, wpflag_is_set, fmt_diskid,
 *                        gen_disk_id, offer_diskid
 *   SEC:COMMANDS         cmd_geometry through cmd_help
 *   SEC:HELPTEXT         print_help (full and minimal variants)
 *   SEC:DISKIO           disk_read_mbr, disk_write_mbr (platform-specific)
 *   SEC:ARGPARSE         Command-line argument parsing (platform-split)
 *   SEC:MAIN             main()
 */

/* ***************************************************************************
 * SEC:PORTABILITY -- conditional includes for POSIX vs Open Watcom
 */
#ifdef __WATCOMC__
#	include <io.h>      /* open, read, write, close -- Watcom POSIX I/O layer */
#	include <fcntl.h>   /* O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC               */
#	include <bios.h>    /* _bios_disk, _bios_timeofday                         */
#	define OPEN_MODE  0 /* Watcom open() ignores mode arg; supply a safe zero  */
#else
#	include <unistd.h>
#	include <fcntl.h>
#	include <sys/stat.h>
#	define OPEN_MODE  0644
#	define O_BINARY   0    /* Define away for POSIX case */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ***************************************************************************
 * SEC:TEMPLATE -- mbr88 blank template, included from generated header
 */

#include "mbr88.h"

/* Catch any future mismatch between size constants at compile time */
typedef char mbr_size_check_[
	(MBR88_TEMPLATE_SIZE == 512) ? 1 : -1
];

/* ***************************************************************************
 * SEC:CONSTANTS
 */

#define MBR_SIZE          512
#define PTABLE_OFFSET     0x1BE
#define ENTRY_SIZE        16
#define NUM_ENTRIES       4
#define BOOTSIG_OFFSET    0x1FE

/* MBR88_LABEL_BASE, MBR88_LABEL_SLOT_SZ, MBR88_LABEL_MAX come from mbr88.h */

/* Two-column display geometry -- 38 + 4 + 38 = 80 */
#define COL_WIDTH         37
#define COL_GAP           4
#define LINE_WIDTH        (COL_WIDTH * 2 + COL_GAP)

#define DEFAULT_HEADS     16
#define DEFAULT_SECTORS   17

/* MBR metadata region (standard MBR fields, not mbr88-specific)
 *   0x1B8-0x1BB  32-bit disk ID (little-endian)
 *   0x1BC-0x1BD  write-protect flag: 0x0000=normal, 0x5A5A=read-only hint
 * These fields are managed by the 'm' command and preserved across upgrades. */
#define DISKID_OFFSET     0x1B8
#define DISKID_LEN        4
#define WPFLAG_OFFSET     0x1BC
#define WPFLAG_LEN        2
#define WPFLAG_SET        0x5A5A

/* ***************************************************************************
 * SEC:GLOBALS -- session state; all interactive modes share these
 */

static unsigned char mbr[MBR_SIZE];
static char          mbr_path[256];
static int           file_exists;
static int           has_mbr88_sig;
static int           dirty;
static int           geo_heads;
static int           geo_sectors;

/* ***************************************************************************
 * SEC:TYPETABLE -- partition type name table and lookup
 */

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
	{ 0x80, "ELKS/Old MINIX"   },
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

/* Print partition type list in two columns, no 0x prefix -- the 'l' command
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

/* ***************************************************************************
 * SEC:CHS -- pack/unpack CHS fields and convert CHS to LBA
 */

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

/* ***************************************************************************
 * SEC:LABELS -- build, read, and write mbr88 boot menu label slots
 */

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

/* ***************************************************************************
 * SEC:MBR88DETECT -- signature detection, version check, upgrade to mbr88
 */

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
 * Label slot format is defined for MBR88_VER_BYTE.  Future versions
 * are not supported by this build; the caller should direct the user
 * to get a newer mbrpatch. */
static int labels_supported(void)
{
	return detect_mbr88() && (mbr88_version() == MBR88_VER_BYTE);
}

/* Forward declaration -- ask_yn is defined after the input helpers below,
 * but hostile_write_ok needs it at the point of the detection block. */
static int ask_yn(const char *prompt);

/* Forward declarations -- diskid/wpflag helpers are defined after the
 * upgrade block, but print_table() calls them from the status bar. */
static int  diskid_is_zero(void);
static int  wpflag_is_set(void);
static void fmt_diskid(char *buf);

/* ***************************************************************************
 * SEC:HOSTILEDETECT -- GPT block, GRUB/LILO warn, hostile_write_ok guard
 *
 * Checks run after every MBR load (file or device) before any other
 * processing.  Three response tiers:
 *
 *   HOSTILE_NONE    -- clean, proceed normally
 *   HOSTILE_WARN    -- known third-party boot loader; warn before writes
 *   HOSTILE_BLOCK   -- GPT or hybrid GPT; refuse all write operations
 *
 * Detection methods:
 *
 *   GPT protective MBR:
 *     Any partition entry with type byte 0xEE indicates a GPT disk.
 *     The real partition table is in LBA 1 and not visible here.
 *     Writing anything risks corrupting the GPT header.
 *     If 0xEE appears alongside other non-empty entries it is a hybrid
 *     GPT/MBR, which is fragile and equally off-limits.
 *
 *   GRUB Legacy in MBR:
 *     Byte 0 is 0xEB (short JMP) and the ASCII string "GRUB" appears
 *     in the boot code area above the label region (0x81-0x1BD).
 *     GRUB Legacy places the string at a fixed offset (~0x176-0x17B
 *     depending on version) but scanning the full post-label code
 *     region is more robust across versions and distro patches.
 *
 *   GRUB2 in MBR:
 *     Byte 0 is 0xEB and byte 2 is 0x90 (the FAT BPB NOP convention
 *     that GRUB2 boot.img deliberately mimics), but no "GRUB" string
 *     appears in the boot code area (GRUB2 puts human-readable strings
 *     in core.img, not in the 512-byte MBR sector).
 *
 *   LILO in MBR:
 *     Byte 0 is 0xFA (CLI) and the ASCII string "LILO" appears at
 *     the fixed offset 0x06-0x09 in every known LILO version.
 *
 * Note: mbr88 starts with 0xE9 (near JMP rel16), distinct from both
 * 0xEB (GRUB/LILO short JMP) and 0xFA (LILO CLI).  No collision possible.
 */

#define HOSTILE_NONE   0   /* clean MBR, proceed normally                  */
#define HOSTILE_WARN   1   /* known third-party loader, warn before writes  */
#define HOSTILE_BLOCK  2   /* GPT/hybrid, refuse all write operations       */

/* hostile_loader: set by detect_hostile_mbr(), read by write-path callers.
 * Stored globally so print_table() and write commands can both see it
 * without re-running detection. */
static int hostile_loader = HOSTILE_NONE;

/* hostile_desc: human-readable description set alongside hostile_loader. */
static const char *hostile_desc = NULL;

/*
 * detect_hostile_mbr -- scan the global mbr[] buffer for hostile patterns.
 *
 * Sets hostile_loader and hostile_desc as a side effect.
 * Returns HOSTILE_NONE / HOSTILE_WARN / HOSTILE_BLOCK.
 *
 * Called from load_and_validate() and from the -r post-read block.
 * Safe to call multiple times; always re-evaluates from scratch.
 */
static int detect_hostile_mbr(void)
{
	int i;
	int has_ee   = 0;   /* found a type-0xEE partition entry          */
	int has_real = 0;   /* found a non-empty, non-0xEE entry alongside */

	hostile_loader = HOSTILE_NONE;
	hostile_desc   = NULL;

	/* --- Tier 1: GPT protective / hybrid MBR --- */
	for (i = 0; i < NUM_ENTRIES; i++) {
		const unsigned char *e = mbr + PTABLE_OFFSET + i * ENTRY_SIZE;
		unsigned char ptype = e[4];

		if (ptype == 0xEE) {
			has_ee = 1;
		} else {
			/* Entry is non-empty if any CHS/LBA/type field is set */
			if (ptype != 0x00
				|| e[1] || e[2] || e[3]
				|| e[5] || e[6] || e[7]
				|| e[8] || e[9] || e[10] || e[11])
				has_real = 1;
		}
	}

	if (has_ee && has_real) {
		hostile_loader = HOSTILE_BLOCK;
		hostile_desc   = "hybrid GPT/MBR";
		return HOSTILE_BLOCK;
	}
	if (has_ee) {
		hostile_loader = HOSTILE_BLOCK;
		hostile_desc   = "GPT protective MBR";
		return HOSTILE_BLOCK;
	}

	/* --- Tier 2: LILO --- */
	/* LILO MBR: byte 0 is FA (CLI), "LILO" at fixed offset 0x06. */
	if (mbr[0] == 0xFA
		&& mbr[6]  == 'L' && mbr[7]  == 'I'
		&& mbr[8]  == 'L' && mbr[9]  == 'O') {
		hostile_loader = HOSTILE_WARN;
		hostile_desc   = "LILO boot code";
		return HOSTILE_WARN;
	}

	/* --- Tier 2: GRUB (Legacy and GRUB2) --- */
	/* Both use EB xx at byte 0.  GRUB Legacy has "GRUB" in the code area
	 * above the label region.  GRUB2 has no such string in the MBR sector
	 * but uses EB xx 90 (the FAT BPB NOP convention). */
	if (mbr[0] == 0xEB) {
		/* Scan post-label boot code area for "GRUB" (Legacy) */
		for (i = 0x81; i <= 0x1BD - 3; i++) {
			if (mbr[i]   == 'G' && mbr[i+1] == 'R'
				&& mbr[i+2] == 'U' && mbr[i+3] == 'B') {
				hostile_loader = HOSTILE_WARN;
				hostile_desc   = "GRUB Legacy boot code";
				return HOSTILE_WARN;
			}
		}
		/* No "GRUB" string: if byte 2 is 0x90 it is almost certainly GRUB2 */
		if (mbr[2] == 0x90) {
			hostile_loader = HOSTILE_WARN;
			hostile_desc   = "GRUB2 boot code";
			return HOSTILE_WARN;
		}
		/* EB xx without 0x90 at byte 2 and no GRUB string: some other
		 * short-jump MBR loader.  Warn generically. */
		hostile_loader = HOSTILE_WARN;
		hostile_desc   = "unknown third-party boot loader (short-jump MBR)";
		return HOSTILE_WARN;
	}

	return HOSTILE_NONE;
}

/*
 * hostile_write_ok -- call before any write operation.
 *
 * HOSTILE_BLOCK: print error, return 0 (caller must abort).
 * HOSTILE_WARN:  print warning, ask user to confirm, return their answer.
 * HOSTILE_NONE:  return 1 silently.
 */
static int hostile_write_ok(void)
{
	if (hostile_loader == HOSTILE_BLOCK) {
		fprintf(stderr,
			"Error: this disk has a %s.\n"
			"  mbrpatch is an MBR tool and knows better than to touch this.\n"
			"  Backing away slowly.  Use a GPT-aware tool such as gdisk or parted.\n",
			hostile_desc);
		return 0;
	}
	if (hostile_loader == HOSTILE_WARN) {
		printf(
			"Warning: this MBR contains %s.\n"
			"  Upgrading will overwrite the existing boot loader and the system\n"
			"  may not boot until it is reinstalled.\n",
			hostile_desc);
		return ask_yn("Proceed anyway? (Y/N): ");
	}
	return 1;
}

/*
 * is_valid_label_slot -- check whether a 16-byte label slot contains a
 * plausible mbr88 label, without relying on the mbr88 signature being
 * present (used during -u upgrade to recover labels from any mbr88 image).
 *
 * A valid slot contains:
 *   - 1 to MBR88_LABEL_MAX printable ASCII bytes (0x20-0x7E)
 *   - immediately followed by CR (0x0D) then LF (0x0A) then NUL (0x00)
 *   - all remaining bytes through slot[MBR88_LABEL_SLOT_SZ-1] are 0x00
 *
 * This pattern is tight enough that random boot code bytes are extremely
 * unlikely to pass.  If a false positive does occur the user can simply
 * correct the label during the interactive session.
 */
static int is_valid_label_slot(const unsigned char *slot)
{
	int i;

	/* Scan printable ASCII run; must be at least 1 byte */
	for (i = 0; i < MBR88_LABEL_MAX; i++) {
		if (slot[i] == 0x0D) break;          /* CR ends the text run */
		if (slot[i] < 0x20 || slot[i] > 0x7E)
			return 0;                         /* non-printable, not a label */
	}
	if (i == 0) return 0;                     /* zero-length label, not valid */

	/* CR LF NUL must follow immediately */
	if (slot[i]   != 0x0D) return 0;
	if (slot[i+1] != 0x0A) return 0;
	if (slot[i+2] != 0x00) return 0;

	/* Everything after the NUL must be zero padding */
	for (i = i + 3; i < MBR88_LABEL_SLOT_SZ; i++)
		if (slot[i] != 0x00) return 0;

	return 1;
}

/*
 * upgrade_to_mbr88 -- replace boot code with the mbr88 v0.2 template,
 * preserving the partition table and any recoverable label slots.
 *
 * Label recovery: the label area (MBR88_LABEL_BASE, 64 bytes) exists at
 * the same offset in every mbr88 version.  Before overwriting, each slot
 * is tested with is_valid_label_slot().  Slots that pass are copied back
 * after the template is applied; slots that fail are left zeroed (the
 * template default).  This correctly handles:
 *   - Upgrading a v0.1 mbr88 image: labels recovered.
 *   - Upgrading a generic MBR: label area contains code bytes that will
 *     not pass the slot check, so labels are zeroed.
 */
static void upgrade_to_mbr88(void)
{
	unsigned char old_ptable[64];
	unsigned char old_labels[64];
	unsigned char old_diskid[DISKID_LEN];    /* 0x1B8-0x1BB: disk ID         */
	unsigned char old_wpflag[WPFLAG_LEN];    /* 0x1BC-0x1BD: write-prot flag  */
	int           label_valid[NUM_ENTRIES];
	int           i;

	/* Save partition table */
	memcpy(old_ptable, mbr + PTABLE_OFFSET, 64);

	/* Save metadata fields -- preserved across the template copy so that an
	 * upgrade on a disk that already has an ID does not silently clear it. */
	memcpy(old_diskid, mbr + DISKID_OFFSET, DISKID_LEN);
	memcpy(old_wpflag, mbr + WPFLAG_OFFSET, WPFLAG_LEN);

	/* Save and validate each label slot independently */
	memcpy(old_labels, mbr + MBR88_LABEL_BASE, 64);
	for (i = 0; i < NUM_ENTRIES; i++)
		label_valid[i] = is_valid_label_slot(
			old_labels + i * MBR88_LABEL_SLOT_SZ);

	/* Overwrite with fresh template (zeros label area and boot code) */
	memcpy(mbr, MBR88_TEMPLATE, MBR_SIZE);

	/* Restore partition table */
	memcpy(mbr + PTABLE_OFFSET, old_ptable, 64);

	/* Restore metadata fields */
	memcpy(mbr + DISKID_OFFSET, old_diskid, DISKID_LEN);
	memcpy(mbr + WPFLAG_OFFSET, old_wpflag, WPFLAG_LEN);

	/* Restore valid label slots; invalid slots remain zeroed from template */
	for (i = 0; i < NUM_ENTRIES; i++) {
		if (label_valid[i])
			memcpy(
				mbr + MBR88_LABEL_BASE + i * MBR88_LABEL_SLOT_SZ,
				old_labels + i * MBR88_LABEL_SLOT_SZ,
				MBR88_LABEL_SLOT_SZ
			);
	}
}

/* ***************************************************************************
 * SEC:FILEIO -- copy_file, load_and_validate
 */

static int copy_file(const char *src, const char *dst)
{
	unsigned char buf[512];
	int fdin, fdout, n, result = 0;
	fdin = open(src, O_RDONLY | O_BINARY);
	if (fdin < 0) { perror(src); return -1; }
	fdout = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, OPEN_MODE);
	if (fdout < 0) { perror(dst); close(fdin); return -1; }
	while ((n = read(fdin, buf, sizeof(buf))) > 0) {
		if (write(fdout, buf, n) != n) { perror(dst); result = -1; break; }
	}
	close(fdin);
	close(fdout);
	return result;
}

/* ***************************************************************************
 * SEC:INPUT -- read_line, ask_int, ask_hex, ask_yn, read_diskid, ask_slot
 */

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

/*
 * read_diskid -- prompt for a 32-bit disk ID entered as up to 8 hex digits.
 *
 * Accepts any string of 1-8 hex characters (0-9, a-f, A-F), with or without
 * a leading 0x prefix or a trailing 'h' suffix.  Spaces are ignored so the
 * user can type "1111 1110" the same way fdisk displays it.  The value is
 * stored little-endian at mbr[DISKID_OFFSET..+3].
 *
 * Returns 1 on success, 0 if the input contained non-hex characters or was
 * empty after stripping whitespace and prefix/suffix.
 */
static int read_diskid(void)
{
	char raw[16];
	char hex[10];  /* up to 8 hex digits + NUL */
	int  hlen, i;
	unsigned long val;
	char *p;

	printf("  Enter disk ID (up to 8 hex digits, spaces ok): ");
	fflush(stdout);
	if (!read_line(raw, sizeof(raw))) return 0;

	/* Strip spaces */
	hlen = 0;
	for (p = raw; *p && hlen < 9; p++) {
		if (*p == ' ' || *p == '\t') continue;
		hex[hlen++] = *p;
	}
	hex[hlen] = '\0';

	/* Strip optional 0x prefix */
	p = hex;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		p += 2;
		hlen -= 2;
	}

	/* Strip optional trailing h */
	if (hlen > 0 && (p[hlen-1] == 'h' || p[hlen-1] == 'H')) {
		p[--hlen] = '\0';
	}

	if (hlen == 0 || hlen > 8) return 0;

	/* Validate: all remaining chars must be hex digits */
	for (i = 0; i < hlen; i++) {
		char c = p[i];
		if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))
			return 0;
	}

	val = strtoul(p, NULL, 16);

	/* Store little-endian */
	mbr[DISKID_OFFSET]   = (unsigned char)( val        & 0xFF);
	mbr[DISKID_OFFSET+1] = (unsigned char)((val >>  8) & 0xFF);
	mbr[DISKID_OFFSET+2] = (unsigned char)((val >> 16) & 0xFF);
	mbr[DISKID_OFFSET+3] = (unsigned char)((val >> 24) & 0xFF);
	return 1;
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

/* ***************************************************************************
 * SEC:DISPLAY -- two-column table layout, print_table_gpt, print_table
 */

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
	snprintf(line, COL_WIDTH+1, "%-37.37s", s);
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

/*
 * print_table_gpt -- collapsed display for GPT and hybrid GPT disks.
 *
 * A GPT protective MBR contains one partition entry of type 0xEE that
 * spans the entire disk.  The CHS and LBA fields are stub values chosen
 * by the GPT spec and carry no information useful to the user.  The real
 * partition table lives at LBA 1 and is invisible to mbrpatch.
 *
 * Showing the raw 0xEE entry with CHS/LBA decoded would be actively
 * misleading, so this function suppresses all those fields and replaces
 * the normal table display with a brief explanation and a warning banner.
 *
 * For hybrid MBRs (0xEE alongside real entries) the real entries are
 * shown normally because the user might need to see them, but the
 * warning banner is more alarming.
 */
static void print_table_gpt(int is_hybrid)
{
	int n;
	const char *title = is_hybrid
		? "*** HYBRID GPT/MBR DISK -- DO NOT MODIFY ***"
		: "*** GPT DISK -- READ ONLY ***";
	int len  = (int)strlen(title);
	int pad  = (LINE_WIDTH - len) / 2;
	int rpad = LINE_WIDTH - len - pad;

	for (n = 0; n < LINE_WIDTH; n++) putchar('=');
	putchar('\n');
	printf("%*s%s%*s\n", pad, "", title, rpad, "");
	for (n = 0; n < LINE_WIDTH; n++) putchar('=');
	putchar('\n');

	if (is_hybrid) {
		printf(
			"  This disk has a hybrid GPT/MBR layout.  These are fragile.\n"
			"  The real partition table is in LBA 1 (the GPT header).\n"
			"  The MBR entries below may not match the GPT partition table.\n"
			"  mbrpatch will not write to this disk.\n"
			"  Use a GPT-aware tool such as gdisk or parted.\n");
		for (n = 0; n < LINE_WIDTH; n++) putchar('-');
		putchar('\n');
		/* Show the real (non-0xEE) entries so the user can see what is there,
		 * but label them clearly and suppress any bootable-flag tag. */
		{
			char left[7][COL_WIDTH+1];
			char right[7][COL_WIDTH+1];
			char gap[COL_GAP+1];
			memset(gap, ' ', COL_GAP);
			gap[COL_GAP] = '\0';
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
		}
	} else {
		/* Pure GPT protective MBR: one 0xEE entry, everything else zeroed.
		 * Nothing useful to show from the partition table itself. */
		printf(
			"  This disk uses the GUID Partition Table (GPT) format.\n"
			"  The MBR contains a single protective entry (type 0xEE)\n"
			"  that reserves the entire disk.  The real partition table\n"
			"  is at LBA 1 and is not visible here.\n"
			"  mbrpatch will not write to this disk.\n"
			"  Use a GPT-aware tool such as gdisk or parted.\n");
	}

	for (n = 0; n < LINE_WIDTH; n++) putchar('=');
	putchar('\n');
	{
		const char *base = strrchr(mbr_path, '/');
		base = base ? base + 1 : mbr_path;
		printf("  File: %s  [%s]\n\n", base, hostile_desc);
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

	/* Delegate to collapsed GPT display for BLOCK-tier disks */
	if (hostile_loader == HOSTILE_BLOCK) {
		int is_hybrid = 0;
		int i;
		/* Hybrid: 0xEE present alongside at least one non-empty other entry */
		for (i = 0; i < NUM_ENTRIES; i++) {
			const unsigned char *e = mbr + PTABLE_OFFSET + i * ENTRY_SIZE;
			if (e[4] != 0xEE
				&& (e[4] || e[1] || e[2] || e[3]
					|| e[5] || e[6] || e[7]
					|| e[8] || e[9] || e[10] || e[11]))
				is_hybrid = 1;
		}
		print_table_gpt(is_hybrid);
		return;
	}

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
			printf("  File: %s  %s  %s\n", base, sig_tag, geo_buf);
		else
			printf("  File: %s  %s  Geometry: (use 'g' to set)\n",
				base, sig_tag);
	}

	/* Warn about third-party boot loaders below the status bar */
	if (hostile_loader == HOSTILE_WARN)
		printf("  Warning: MBR contains %s.\n", hostile_desc);

	/* Disk ID and write-protect -- suppress both lines when both are zero
	 * (clean XT disk with no utilities having touched the metadata region). */
	{
		int show_id = !diskid_is_zero();
		int show_wp = wpflag_is_set();
		if (show_id || show_wp) {
			if (show_id) {
				char idbuf[10];
				fmt_diskid(idbuf);
				printf("  Disk ID: %s", idbuf);
				if (show_wp) printf("    Write-protect: SET");
				putchar('\n');
			} else {
				printf("  Write-protect: SET\n");
			}
		}
	}

	putchar('\n');
}

/* ***************************************************************************
 * SEC:DISKID -- disk ID and write-protect flag helpers
 */

/*
 * diskid_is_zero -- return 1 if the disk ID field is all zeros.
 * Used to decide whether to show "(not set)" and whether to offer
 * auto-generation on -n / -u entry.
 */
static int diskid_is_zero(void)
{
	int i;
	for (i = 0; i < DISKID_LEN; i++)
		if (mbr[DISKID_OFFSET + i] != 0) return 0;
	return 1;
}

/*
 * wpflag_is_set -- return 1 if the write-protect flag equals 0x5A5A.
 */
static int wpflag_is_set(void)
{
	return (mbr[WPFLAG_OFFSET]   == 0x5A)
	    && (mbr[WPFLAG_OFFSET+1] == 0x5A);
}

/*
 * fmt_diskid -- format the four disk ID bytes as "XXXX XXXX" into buf.
 * buf must be at least 10 bytes.
 */
static void fmt_diskid(char *buf)
{
	sprintf(buf, "%02X%02X %02X%02X",
		mbr[DISKID_OFFSET+3], mbr[DISKID_OFFSET+2],
		mbr[DISKID_OFFSET+1], mbr[DISKID_OFFSET]);
}

/*
 * gen_disk_id -- fill mbr[DISKID_OFFSET..+3] with a generated disk ID.
 *
 * On Linux (native): reads 4 bytes from /dev/urandom.  This gives adequate
 * collision avoidance for the intended use (a few disks owned by one user).
 *
 * On ELKS (ia16-elf-gcc / __ia16__): reads the BIOS tick counter via
 * INT 1Ah/00h.  The counter increments ~18.2 times per second from power-on.
 * We XOR the high and low halves of the 32-bit tick count together and XOR
 * again with a fixed constant to make the result less boring for fresh boots.
 * Not cryptographic, but plenty of entropy for distinguishing a handful of
 * disks.
 *
 * On FreeDOS (Open Watcom / __WATCOMC__): same BIOS tick approach using
 * _bios_timeofday() from <bios.h> -- no inline assembly needed.
 *
 * Returns 1 on success, 0 on failure (e.g. /dev/urandom unavailable).
 */
static int gen_disk_id(void)
{
#if defined(__WATCOMC__)

	/* FreeDOS: use BIOS time-of-day tick counter via Open Watcom <bios.h>.
	 * _bios_timeofday(_TIME_GETCLOCK, &ticks) stores the 32-bit tick count
	 * in the low unsigned long and returns 0 normally (1 = midnight rollover,
	 * which is harmless here).  We split the 32-bit value and XOR the halves
	 * for a 32-bit ID with somewhat unpredictable bit patterns. */
	{
		unsigned long ticks = 0;
		unsigned long hi, lo, id;
		_bios_timeofday(_TIME_GETCLOCK, (long*)&ticks);
		hi = (ticks >> 16) & 0xFFFF;
		lo = ticks & 0xFFFF;
		id = ((hi ^ 0xA55A) << 16) | (lo ^ 0x5AA5);
		mbr[DISKID_OFFSET]   = (unsigned char)(id & 0xFF);
		mbr[DISKID_OFFSET+1] = (unsigned char)((id >>  8) & 0xFF);
		mbr[DISKID_OFFSET+2] = (unsigned char)((id >> 16) & 0xFF);
		mbr[DISKID_OFFSET+3] = (unsigned char)((id >> 24) & 0xFF);
		return 1;
	}

#elif defined(__ia16__)

	/* ELKS: inline assembly for INT 1Ah/00h (read timer tick count).
	 * On return: CX = high word, DX = low word of 32-bit tick count.
	 * We XOR the halves and fold in a constant to spread the bits. */
	{
		unsigned int tick_hi, tick_lo;
		unsigned long id;
		__asm__ __volatile__ (
			"int $0x1A"
			: "=c" (tick_hi), "=d" (tick_lo)
			: "0" (0), "1" (0), "a" (0x0000)
			: "bx"
		);
		id = ((unsigned long)(tick_hi ^ 0xA55A) << 16)
		   | ((unsigned long)(tick_lo ^ 0x5AA5) & 0xFFFF);
		mbr[DISKID_OFFSET]   = (unsigned char)(id & 0xFF);
		mbr[DISKID_OFFSET+1] = (unsigned char)((id >>  8) & 0xFF);
		mbr[DISKID_OFFSET+2] = (unsigned char)((id >> 16) & 0xFF);
		mbr[DISKID_OFFSET+3] = (unsigned char)((id >> 24) & 0xFF);
		return 1;
	}

#else

	/* Linux (native): read 4 bytes from /dev/urandom.
	 * This is the best available entropy source on a full Linux system. */
	{
		int fd, n;
		fd = open("/dev/urandom", O_RDONLY|O_BINARY);
		if (fd < 0) return 0;
		n = read(fd, mbr + DISKID_OFFSET, DISKID_LEN);
		close(fd);
		return (n == DISKID_LEN) ? 1 : 0;
	}

#endif
}

/*
 * offer_diskid -- prompt the user to generate a disk ID when none is set.
 * Called on -n and -u entry if the disk ID field is zero.
 * Sets dirty if the user accepts.
 */
static void offer_diskid(void)
{
	if (!diskid_is_zero()) return;

	printf("  No disk ID set.  Disk IDs help utilities identify this disk\n");
	printf("  and are harmless on XT-class hardware.\n");
	if (ask_yn("  Generate one now? (Y/N): ")) {
		if (gen_disk_id()) {
			char buf[10];
			fmt_diskid(buf);
			printf("  Disk ID set to %s.\n", buf);
			dirty = 1;
		} else {
			printf("  Could not generate a disk ID (entropy source unavailable).\n");
		}
	}
}

/* ***************************************************************************
 * SEC:COMMANDS -- cmd_geometry, cmd_new, cmd_delete, cmd_set_type,
 *                cmd_bootable, cmd_label, cmd_metadata, cmd_types,
 *                cmd_write, cmd_help
 */

/* bios_get_drive_params -- query INT 13h/08h for one drive.
 *
 * Compiled only for 16-bit real-mode targets (ELKS and FreeDOS/Watcom).
 * On both targets we use inline assembly rather than Watcom's _bios_disk(),
 * because _bios_disk() does not wrap INT 13h/08h.  The Watcom __asm block
 * and the GCC-style __asm__ block are dialect-separated by #ifdef but the
 * surrounding logic is shared.
 *
 * INT 13h, AH=08h: Get Drive Parameters
 *   In:  AH = 08h, DL = drive number (80h-FFh for hard disks)
 *   Out: CF set on error; AH = status (non-zero = error)
 *        CH = low 8 bits of max cylinder number
 *        CL bits 5-0 = max sector number (1-based)
 *        CL bits 7-6 = high 2 bits of max cylinder (gives 10-bit total)
 *        DH = max head number (0-based, so actual count = DH + 1)
 *        DL = number of drives on this controller
 *
 * We push/pop ES and DI because INT 13h/08h may trash them on some BIOSes
 * (documented behaviour for floppy drives; harmless precaution for hard
 * disks).
 *
 * Returns 1 on success, 0 on BIOS error.
 * *max_cyl, *max_head are 0-based; *max_sec is 1-based (sector numbering
 * starts at 1 in CHS).  Caller adds 1 to cyl/head for the actual count.
 */
#if defined(__WATCOMC__) || defined(__ia16__)

static int bios_get_drive_params(
	unsigned char drive,
	int *max_cyl, int *max_head, int *max_sec, int *num_drives)
{
	unsigned int ax_out, cx_out, dx_out, carry;

#if defined(__WATCOMC__)
	/* Open Watcom inline assembly.  SBB reg,reg sets reg to 0xFFFF if
	 * carry is set, 0 if clear -- used here to capture the CF state
	 * before any other instruction can disturb it. */
   __asm {
      push es
      push di
      mov  ax, 0x0800
      mov  dl, drive
      int  0x13
      mov  ax_out, ax
      mov  cx_out, cx
      mov  dx_out, dx
      mov  bx, 0
      sbb  bx, bx        /* BX = 0xFFFF if CF set, 0 if clear */
      mov  carry, bx
      pop  di
      pop  es
   }
#else
	/* ia16-elf-gcc GCC-style inline assembly.
	 * The carry flag is captured into 'carry' via SBB; the other output
	 * registers are pinned with explicit constraints.  AH is in the high
	 * byte of ax_out; we check it separately for non-zero error status. */
	__asm__ __volatile__ (
		"push %%es\n\t"
		"push %%di\n\t"
		"int  $0x13\n\t"
		"sbb  %[car], %[car]\n\t"
		"pop  %%di\n\t"
		"pop  %%es"
		: "=a" (ax_out), "=c" (cx_out), "=d" (dx_out), [car] "=r" (carry)
		: "0" ((unsigned int)0x0800), "2" ((unsigned int)drive)
		: "bx", "memory"
	);
#endif

	/* Treat carry set OR AH != 0 as failure */
	if (carry || (ax_out & 0xFF00))
		return 0;

	/* Unpack: cylinder is 10 bits: CH = low 8, CL[7:6] = high 2 */
	*max_cyl    = (int)(((cx_out >> 8) & 0xFF) | ((cx_out & 0xC0) << 2));
	*max_sec    = (int)(cx_out & 0x3F);           /* 1-based sector count */
	*max_head   = (int)((dx_out >> 8) & 0xFF);    /* 0-based head max     */
	*num_drives = (int)(dx_out & 0xFF);
	return 1;
}

#endif /* __WATCOMC__ || __ia16__ */

static void cmd_geometry(void)
{
	printf("\n");
	printf("Drive geometry converts CHS addresses to LBA sector numbers.\n\n");

#if defined(__WATCOMC__) || defined(__ia16__)
	/* On 16-bit targets, offer a live BIOS query before manual entry.
	 * The user opted in, so the label on the output is the only context
	 * needed -- no separate disclaimer paragraph required. */
	if (ask_yn("Show BIOS-reported geometry for this machine? (Y/N): ")) {
		int drive, max_cyl, max_head, max_sec, num_drives;
		int found  = 0;
		char choice_buf[8];

		printf("\n  BIOS geometry (this machine):\n");
		printf("  %-8s  %-10s  %-8s  %-8s\n",
			"Drive", "Cylinders", "Heads", "Sec/Trk");
		printf("  --------  ----------  --------  --------\n");

		/* Probe drives 1 and 2 (BIOS 80h, 81h).  Stopping at 81h is
		 * intentional: probing beyond 81h hangs some BIOSes, and the
		 * partition table only addresses one disk at a time anyway. */
		for (drive = 0x80; drive <= 0x81; drive++) {
			int ok = bios_get_drive_params(
				(unsigned char)drive,
				&max_cyl, &max_head, &max_sec, &num_drives);
			if (!ok) break;   /* no more drives on this controller */
			/* Add 1 to 0-based cyl and head counts for display */
			printf("  Drive %-2d  %-10d  %-8d  %-8d\n",
				drive - 0x7F,   /* 0x80->1, 0x81->2 */
				max_cyl  + 1,
				max_head + 1,
				max_sec);
			found = 1;
			if (num_drives <= 1) break;  /* BIOS says only one drive */
		}

		if (!found) {
			printf("  (BIOS reported no hard drives)\n");
		} else {
			int chosen_h = 0, chosen_s = 0;

			printf("\n");
			printf("  Enter drive number (1 or 2), or press Enter for manual entry: ");
			fflush(stdout);

			if (read_line(choice_buf, sizeof(choice_buf))
					&& choice_buf[0]) {
				/* Accept ordinal (1/2) or legacy hex (80h/81h) */
				int drv = disk_parse_drive(choice_buf);
				if (drv == 0x80 || drv == 0x81) {
					int ok = bios_get_drive_params(
						(unsigned char)drv,
						&max_cyl, &max_head, &max_sec,
						&num_drives);
					if (ok) {
						chosen_h = max_head + 1;
						chosen_s = max_sec;
					} else {
						printf("  Could not read Drive %d (BIOS %02Xh).\n",
							drv - 0x7F, drv);
					}
				} else {
					printf("  Unrecognised drive -- using manual entry.\n");
				}
			}

			if (chosen_h) {
				/* Confirm or let the user override each field */
				printf("\n");
				printf("  Heads per cylinder [%d]: ", chosen_h);
				fflush(stdout);
				read_line(choice_buf, sizeof(choice_buf));
				if (choice_buf[0]) {
					int v = atoi(choice_buf);
					if (v >= 1 && v <= 255) chosen_h = v;
					else printf("  Out of range -- keeping %d.\n", chosen_h);
				}

				printf("  Sectors per track  [%d]: ", chosen_s);
				fflush(stdout);
				read_line(choice_buf, sizeof(choice_buf));
				if (choice_buf[0]) {
					int v = atoi(choice_buf);
					if (v >= 1 && v <= 63) chosen_s = v;
					else printf("  Out of range -- keeping %d.\n", chosen_s);
				}

				geo_heads   = chosen_h;
				geo_sectors = chosen_s;
				printf("  Geometry set: %dH / %dS.\n",
					geo_heads, geo_sectors);
				return;
			}
			/* User pressed Enter -- fall through to manual entry below */
		}
	}
	printf("\n");
#endif /* __WATCOMC__ || __ia16__ */

	/* Manual entry -- all platforms, and fallback when BIOS skipped/failed */
	geo_heads   = ask_int("  Heads per cylinder (1-255): ", 1, 255);
	geo_sectors = ask_int("  Sectors per track  (1-63):  ", 1, 63);
	printf("  Geometry set: %dH / %dS.\n", geo_heads, geo_sectors);
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
		return;   /* no redraw -- nothing changed */
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
	print_table();   /* redraw here; main loop must not also redraw for 'n' */
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

/*
 * cmd_metadata -- 'm' command: manage disk ID (0x1B8-0x1BB) and
 * write-protect flag (0x1BC-0x1BD).
 *
 * These fields live in the standard MBR region and are available for
 * editing regardless of whether the mbr88 signature is present.  The
 * disk ID is a 32-bit collision-avoidance identifier used by disk
 * utilities; the write-protect flag is a hint (rarely enforced) that
 * the disk is read-only.
 *
 * The submenu is context-sensitive: (S)et and (C)lear are shown only
 * when relevant to the current flag state, to avoid presenting a no-op.
 * (E)nter allows the user to set a specific disk ID value directly,
 * which is useful when a recognizable or meaningful value is desired.
 */
static void cmd_metadata(void)
{
	char buf[8];
	char idbuf[10];
	int  wp;

	for (;;) {
		/* Show current state */
		printf("\n");
		if (diskid_is_zero())
			printf("  Disk ID      : not set\n");
		else {
			fmt_diskid(idbuf);
			printf("  Disk ID      : %s\n", idbuf);
		}
		wp = wpflag_is_set();
		printf("  Write-protect: %s\n", wp ? "SET" : "not set");
		printf("\n");

		/* Context-sensitive submenu */
		printf("  (G) Generate new disk ID\n");
		printf("  (E) Enter disk ID manually\n");
		printf("  (Z) Zero the disk ID\n");
		if (!wp)
			printf("  (S) Set write-protect\n");
		else
			printf("  (C) Clear write-protect\n");
		printf("  (K) Keep / exit\n");
		printf("  Choice: "); fflush(stdout);

		if (!read_line(buf, sizeof(buf))) break;
		if (!buf[0]) continue;

		switch (buf[0]) {
		case 'g': case 'G':
			if (gen_disk_id()) {
				fmt_diskid(idbuf);
				printf("  Disk ID set to %s.\n", idbuf);
				dirty = 1;
			} else {
				printf("  Could not generate a disk ID (entropy source "
					"unavailable).\n");
			}
			break;

		case 'e': case 'E':
			if (read_diskid()) {
				fmt_diskid(idbuf);
				printf("  Disk ID set to %s.\n", idbuf);
				dirty = 1;
			} else {
				printf("  Invalid input -- enter up to 8 hex digits "
					"(e.g. 11111110).\n");
			}
			break;

		case 'z': case 'Z':
			memset(mbr + DISKID_OFFSET, 0, DISKID_LEN);
			printf("  Disk ID cleared.\n");
			dirty = 1;
			break;

		case 's': case 'S':
			if (wp) {
				printf("  Write-protect is already set.  "
					"Use 'C' to clear it.\n");
			} else {
				mbr[WPFLAG_OFFSET]   = 0x5A;
				mbr[WPFLAG_OFFSET+1] = 0x5A;
				printf("  Write-protect flag set (0x5A5A).\n");
				dirty = 1;
				wp = 1;
			}
			break;

		case 'c': case 'C':
			if (!wp) {
				printf("  Write-protect is not set.  "
					"Use 'S' to set it.\n");
			} else {
				mbr[WPFLAG_OFFSET]   = 0x00;
				mbr[WPFLAG_OFFSET+1] = 0x00;
				printf("  Write-protect flag cleared.\n");
				dirty = 1;
				wp = 0;
			}
			break;

		case 'k': case 'K':
			return;

		default:
			printf("  Unknown option '%c'.\n", buf[0]);
			break;
		}
	}
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

	if (!hostile_write_ok())
		return;

	/* No full redraw here -- the table was already shown after the last
	 * modifying command.  Just confirm and write. */
	active = 0;
	for (i = 0; i < NUM_ENTRIES; i++)
		if (mbr[PTABLE_OFFSET + i * ENTRY_SIZE + 4] != 0)
			active++;

	printf("  %d defined partition(s).  Write changes to '%s'? (Y/N): ",
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
#if defined(__WATCOMC__) || defined(__ia16__)
		/* 8.3 filename constraint: strip any existing extension before
		 * appending .bak.  "mbr.bin" -> "mbr.bak", "mbr" -> "mbr.bak".
		 * Search only the filename part (after last slash or backslash)
		 * so a dot in a directory name is not mistaken for an extension. */
		{
			char base[260];
			char *p, *last_sep, *last_dot;
			strncpy(base, mbr_path, sizeof(base) - 1);
			base[sizeof(base) - 1] = '\0';
			last_sep = NULL;
			for (p = base; *p; p++)
				if (*p == '/' || *p == '\\') last_sep = p;
			last_dot = NULL;
			for (p = last_sep ? last_sep + 1 : base; *p; p++)
				if (*p == '.') last_dot = p;
			if (last_dot) *last_dot = '\0';
			snprintf(bak, sizeof(bak), "%s.bak", base);
		}
#else
		snprintf(bak, sizeof(bak), "%s.bak", mbr_path);
#endif
		if (copy_file(mbr_path, bak) == 0)
			printf("  Backup written to: %s\n", bak);
		else {
			fprintf(stderr, "Error: could not write backup.\n");
			return;
		}
	}

	fd = open(mbr_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, OPEN_MODE);
	if (fd < 0) {
		perror(mbr_path);
		fprintf(stderr, "  !!! Changes were NOT written.\n");
		return;
	}
	n = write(fd, mbr, MBR_SIZE);
	close(fd);
	if (n != MBR_SIZE) {
		fprintf(stderr, "Error: short write.\n");
		fprintf(stderr, "  !!! Changes were NOT written.\n");
		return;
	}

	printf("  Written to '%s'.  %d active, %d empty.\n",
		mbr_path, active, NUM_ENTRIES - active);
	puts("  Use 'q' to quit or other commands to continue editing.");
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
	printf("    m  Set disk metadata (ID, write-protect); 'm' submenu allows\n");
	printf("       generate, enter manually, zero, set/clear write-protect\n");
	printf("    p  Print the partition table\n");
	printf("    l  List common partition type codes\n");
	printf("    w  Write changes to file\n");
	printf("    h  This help text\n");
	printf("    q  Quit (prompts if there are unsaved changes)\n");
	putchar('\n');
}

/* ***************************************************************************
 * SEC:HELPTEXT -- print_help (full for native builds, minimal for targets)
 */

#if defined(__ia16__) || defined(__WATCOMC__)

static void print_help(void)
{
	puts("mbrpatch <file>                   view MBR image file");
#ifdef __WATCOMC__
	puts("mbrpatch /P <file>                patch MBR image file");
	puts("mbrpatch /U <file>                upgrade MBR image to mbr88");
	puts("mbrpatch /N <file>                create new blank mbr88 image");
	puts("mbrpatch /R <file> <drive>        read disk MBR to file");
	puts("  drive: 1=hard disk 1, 2=hard disk 2 (or legacy: 80h, 81h)");
	puts("mbrpatch /W <file> <drive>        write file to disk MBR");
	puts("mbrpatch /?                       this help text");
#else
	puts("mbrpatch -p <file>                patch MBR image file");
	puts("mbrpatch -u <file>                upgrade MBR image to mbr88");
	puts("mbrpatch -n <file>                create new blank mbr88 image");
	puts("mbrpatch -r <file> <device>       read disk MBR to file");
	puts("mbrpatch -w <file> <device>       write file to disk MBR");
#endif
	puts("Use -h on a native system for full help.");
}

#else

static void print_help(void)
{
	puts("mbrpatch -- traditional MBR partition table viewer and editor");
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
	puts("             mbr88 v" MBR88_VER_STR " images.  Other mbr88 versions show a message");
	puts("             directing you to get a newer mbrpatch.");
	puts("");
	puts("  -u         Upgrade.  Replace the boot code of an existing MBR with");
	puts("             the mbr88 v" MBR88_VER_STR " boot record, preserving the partition");
	puts("             table entries.  Existing MBR88 labels are recovered if");
	puts("             the label slots contain valid data.  The file must exist");
	puts("             and be valid.  Label editing is always available after upgrade.");
	puts("");
	puts("  -n         New.  Create a blank mbr88 v" MBR88_VER_STR " image from scratch.");
	puts("             The target file must NOT exist (safety check).  Enters");
	puts("             the same interactive session as -p with labels enabled.");
	puts("");
	puts("  -r         Read disk MBR.  Reads the first sector of <device>,");
	puts("             displays the partition table, and saves it to <mbr_file>.");
	puts("             The output file must not already exist (safety check).");
	puts("             Device is a path to the block device, e.g. /dev/sda.");
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
	puts("  m  Set disk metadata        p  Print table");
	puts("  l  List type codes          w  Write to file");
	puts("  q  Quit");
	puts("");
	puts("MBR88 bootable flag:");
	puts("  mbr88 only shows bootable partitions in the boot menu.");
	puts("  Unlike most MBR loaders, MBR88 allows multiple partitions to be");
	puts("  bootable at once -- all will appear in the menu.");
	puts("  Use the 'b' command to toggle the flag on each partition.");
	puts("");
	puts("MBR88 label editing:");
	puts("  Labels are stored in the MBR at offset 0x41 and displayed in the");
	puts("  boot menu next to the partition number.  They are only supported");
	puts("  for mbr88 v" MBR88_VER_STR " images.  If a newer mbr88 version is detected, you'll");
	puts("  need to get an updated mbrpatch from: https://github.com/cpiker/mbr88");
	puts("");
	puts("Disk metadata ('m' command):");
	puts("  The 'm' command manages two standard MBR fields available on any");
	puts("  traditional MBR disk, not just mbr88.  It works in all interactive");
	puts("  modes (-p, -u, -n).");
	puts("");
	puts("  Disk ID (0x1B8-0x1BB): a 32-bit identifier used by disk utilities");
	puts("  to track disks across reboots and drive letter changes.  It does");
	puts("  not need to be cryptographically random -- just probably unique");
	puts("  across the disks you own.  mbr88 leaves it clear by default.");
	puts("  Submenu options: (G)enerate random ID, (E)nter a value manually,");
	puts("  (Z)ero the ID.  On -n and -u entry, mbrpatch offers to generate");
	puts("  one automatically if the field is empty.");
	puts("");
	puts("  Write-protect flag (0x1BC-0x1BD): a hint value of 0x5A5A marks");
	puts("  the disk as read-only.  Rarely enforced by any tool in practice,");
	puts("  but defined in the standard.  0x0000 = normal.");
	puts("");
	puts("Examples:");
	puts("  mbrpatch mbr.bin                    view current partition table");
	puts("  mbrpatch -r backup.bin /dev/sda     read live MBR to file");
	puts("  mbrpatch -u mbr.bin                 upgrade to MBR88, then edit");
	puts("  mbrpatch -w mbr.bin /dev/sda        write MBR to disk");
	puts("  mbrpatch -n new.bin                 create a fresh MBR88 image");
}

#endif /* __ia16__ || __WATCOMC__ */

/* ***************************************************************************
 * SEC:DISKIO -- disk_read_mbr, disk_write_mbr (platform-specific)
 *
 * On Linux / ELKS the caller passes a device path string (/dev/sda etc.).
 * On FreeDOS (Open Watcom) the caller passes a drive specifier string.
 * Preferred form: "1" (first hard disk) or "2" (second hard disk).
 * Legacy hex forms "80h" / "0x80" are also accepted; disk_parse_drive()
 * handles all forms and maps them to the BIOS drive byte (0x80 / 0x81).
 */

#if defined(__WATCOMC__) || defined(__ia16__)

/* Parse a drive specifier from a string.
 *
 * Pure C -- no platform-specific calls.  Shared by both 16-bit targets:
 * cmd_geometry() (WATCOMC + ia16) and disk_read/write_mbr (WATCOMC only).
 *
 * Preferred (user-facing) forms:
 *   "1"          -> 0x80  (first hard disk, BIOS drive 80h)
 *   "2"          -> 0x81  (second hard disk, BIOS drive 81h)
 *
 * Legacy forms accepted for backward compatibility:
 *   "80h"/"80H"  hex with trailing h suffix
 *   "0x80"/"81h" C-style hex prefix
 *   "128"/"129"  plain decimal BIOS drive number
 *
 * Returns the BIOS drive byte (0x80-0xFF), or -1 if unparseable. */
static int disk_parse_drive(const char *s)
{
	char buf[8];
	int  i, len;
	long val;
	char *end;

	len = (int)strlen(s);
	if (len == 0 || len > 6) return -1;

	/* Ordinal shorthand: "1" and "2" map directly to BIOS drive bytes.
	 * Check before the general hex/decimal path to keep the common case fast
	 * and to avoid "1" being parsed as decimal 1 (which is below 0x80). */
	if (len == 1 && s[0] == '1') return 0x80;
	if (len == 1 && s[0] == '2') return 0x81;

	/* Legacy hex/decimal path: copy to mutable buffer, strip trailing h/H */
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

#endif /* __WATCOMC__ || __ia16__ */

#ifdef __WATCOMC__

static int disk_read_mbr(const char *device, unsigned char *buf)
{
	struct diskinfo_t di;
	unsigned int      rc;
	int               drive;

	drive = disk_parse_drive(device);
	if (drive < 0) {
		fprintf(stderr,
			"Error: '%s' is not a valid drive specifier.\n"
			"  Use 1 for the first hard disk, 2 for the second.\n"
			"  Legacy BIOS forms (80h, 81h) are also accepted.\n",
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
			"Error: '%s' is not a valid drive specifier.\n"
			"  Use 1 for the first hard disk, 2 for the second.\n"
			"  Legacy BIOS forms (80h, 81h) are also accepted.\n",
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
	fd = open(device, O_RDONLY|O_BINARY);
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
	fd = open(device, O_WRONLY|O_BINARY);
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

	fd = open(mbr_path, O_RDONLY|O_BINARY);
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
	detect_hostile_mbr();   /* sets hostile_loader / hostile_desc globals */
	return 0;
}

/* ***************************************************************************
 * SEC:MAIN
 */

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


#ifdef __WATCOMC__
	setmode(fileno(stdout), O_TEXT);
	setmode(fileno(stderr), O_TEXT);
#endif

/* ***************************************************************************
 * SEC:ARGPARSE -- Command-line argument parsing (platform-split)
 *
 * Switch prefix convention:
 *   Linux / ELKS: '-' only  (e.g. -r, -w, -p, -u, -n, -h)
 *   FreeDOS:      '/' preferred (e.g. /R, /W, /P, /U, /N, /?)
 *                 '-' also accepted on FreeDOS for familiarity
 *
 * Switch letter matching is case-insensitive on all platforms.
 * The non-canonical case (uppercase on Linux, lowercase on DOS) works
 * silently but is not shown in any help text.
 *
 * Argument order for all modes:  mbrpatch <flag> <mbr_file> [<device>]
 * The -r / -w (or /R / /W) modes take file then device; all others take
 * file only.
 */

	/* Help check: must come before the general switch parse.
	 *   Linux / ELKS: -h  --help
	 *   FreeDOS:      /?  /H  (also /h, -h, --help accepted silently) */
	if (argc == 2) {
		const char *a = argv[1];
#ifdef __WATCOMC__
		if ((a[0]=='/' && (a[1]=='?' || a[1]=='H' || a[1]=='h') && a[2]=='\0') ||
		    (a[0]=='-' && (a[1]=='h' || a[1]=='H') && a[2]=='\0') ||
		    strcmp(a,"--help")==0) {
#else
		if ((a[0]=='-' && (a[1]=='h' || a[1]=='H') && a[2]=='\0') ||
		    strcmp(a,"--help")==0) {
#endif
			print_help();
			return 0;
		}
	}

#ifdef __WATCOMC__
	/* FreeDOS argument parsing.
	 * Accepts '/' or '-' prefix; switch letter is matched case-insensitively.
	 * Canonical DOS form uses uppercase (/R, /W, /P, /U, /N, /?). */
	if (argc >= 2 &&
	    (argv[1][0] == '/' || argv[1][0] == '-') &&
	    argv[1][1] != '\0') {
		char sw = argv[1][1];
		/* Fold to lowercase for the comparison */
		if (sw >= 'A' && sw <= 'Z') sw = sw + ('a' - 'A');
		switch (sw) {
		case 'r':
		case 'w':
			if (argc != 4) {
				fprintf(stderr,
					"Usage: mbrpatch %c%c <mbr_file> <drive>\n",
					argv[1][0], argv[1][1]);
				return 1;
			}
			if (sw == 'r') mode_read  = 1;
			else            mode_write = 1;
			strncpy(mbr_path,    argv[2], sizeof(mbr_path)    - 1);
			strncpy(disk_device, argv[3], sizeof(disk_device) - 1);
			mbr_path[sizeof(mbr_path)-1]       = '\0';
			disk_device[sizeof(disk_device)-1] = '\0';
			break;
		case 'p':
		case 'u':
		case 'n':
			if (argc != 3) {
				fprintf(stderr,
					"Usage: mbrpatch %c%c <mbr_file>\n",
					argv[1][0], argv[1][1]);
				return 1;
			}
			if (sw == 'p') mode_patch   = 1;
			if (sw == 'u') mode_upgrade = 1;
			if (sw == 'n') mode_new     = 1;
			strncpy(mbr_path, argv[2], sizeof(mbr_path) - 1);
			mbr_path[sizeof(mbr_path)-1] = '\0';
			break;
		default:
			fprintf(stderr, "Unknown option '%s'.\n", argv[1]);
			fprintf(stderr,
				"Usage: mbrpatch [/P|/U|/N] <mbr_file>\n"
				"       mbrpatch /R|/W <mbr_file> <drive>\n"
				"       mbrpatch /?\n");
			return 1;
		}
	} else if (argc == 2) {
		mode_view = 1;
		strncpy(mbr_path, argv[1], sizeof(mbr_path) - 1);
		mbr_path[sizeof(mbr_path)-1] = '\0';
	} else {
		fprintf(stderr,
			"Usage: mbrpatch [/P|/U|/N] <mbr_file>\n"
			"       mbrpatch /R|/W <mbr_file> <drive>\n"
			"       mbrpatch /?\n");
		return 1;
	}

#else  /* Linux / ELKS: '-' prefix, case-insensitive switch letter */

	if (argc >= 2 && argv[1][0] == '-' && argv[1][1] != '\0') {
		char sw = argv[1][1];
		/* Fold to lowercase for the comparison */
		if (sw >= 'A' && sw <= 'Z') sw = sw + ('a' - 'A');
		switch (sw) {
		case 'r':
		case 'w':
			if (argc != 4) {
				fprintf(stderr,
					"Usage: mbrpatch %s <mbr_file> <device>\n",
					argv[1]);
				return 1;
			}
			if (sw == 'r') mode_read  = 1;
			else            mode_write = 1;
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
			if (sw == 'p') mode_patch   = 1;
			if (sw == 'u') mode_upgrade = 1;
			if (sw == 'n') mode_new     = 1;
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

#endif /* __WATCOMC__ */

	/* ******************************************************************
	 * Mode -r: read first sector from disk, validate, display, save
	 */
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

		detect_hostile_mbr();   /* sets hostile_loader / hostile_desc globals */
		has_mbr88_sig = detect_mbr88();
		print_table();

		/* Save to file -- must not already exist (safety) */
		fd2 = open(mbr_path, O_RDONLY|O_BINARY);
		if (fd2 >= 0) {
			close(fd2);
			fprintf(stderr,
				"Error: '%s' already exists.  Remove it first or choose a\n"
				"  different filename to avoid overwriting an existing backup.\n",
				mbr_path);
			return 1;
		}
		fd2 = open(mbr_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, OPEN_MODE);
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

	/* ******************************************************************
	 * Mode -w: load file, display it, confirm, write to disk
	 */
	if (mode_write) {
		if (load_and_validate() != 0)
			return 1;

		has_mbr88_sig = detect_mbr88();
		print_table();

		if (!hostile_write_ok())
			return 1;

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

	/* ******************************************************************
	 * Mode: view -- read, validate, print, exit
	 */
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

	/* ******************************************************************
	 * Mode: new -- file must NOT exist
	 */
	if (mode_new) {
		fd = open(mbr_path, O_RDONLY|O_BINARY);
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
		puts("New mbr88 v" MBR88_VER_STR " image (blank partition table).");
		puts("  Use 'g' then 'n' to define partitions, 'v' for labels, 'w' to write.");
		offer_diskid();
		print_table();
		/* fall through to command loop */
	}

	/* ******************************************************************
	 * Modes: patch / upgrade -- file must exist and be valid
	 */
	if (mode_patch || mode_upgrade) {
		if (load_and_validate() != 0)
			return 1;

		/* Hard block for GPT/hybrid -- refuse interactive editing entirely */
		if (hostile_loader == HOSTILE_BLOCK) {
			print_table();   /* shows the GPT warning display */
			return 1;
		}

		if (mode_upgrade) {
			/* Warn about third-party loaders before overwriting their code */
			if (!hostile_write_ok())
				return 1;
			upgrade_to_mbr88();
			has_mbr88_sig = 1;
			dirty = 1;   /* boot code was replaced -- treat as unsaved change */
			puts("Upgrade: boot code replaced with mbr88 v" MBR88_VER_STR ", "
				"partition table preserved.");
			puts("  Any valid mbr88 labels found in the source image were recovered.");
			puts("  Use 'v' to set or correct labels, 'w' to write when done.");
			offer_diskid();
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
				puts("Generic MBR partition table editing only.");
				puts("  Use -u to upgrade to mbr88 and enable label editing.");
			}
		}
		print_table();
	}

	/* ******************************************************************
	 * Command loop -- shared by -p, -u, -n
	 */
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
		case 'n': case 'N':  cmd_new();               break;
		case 'd': case 'D':  cmd_delete();   redraw=1; break;
		case 't': case 'T':  cmd_set_type(); redraw=1; break;
		case 'b': case 'B':  cmd_bootable(); redraw=1; break;
		case 'v': case 'V':  cmd_label();    redraw=1; break;
		case 'm': case 'M':  cmd_metadata(); redraw=1; break;
		case 'p': case 'P':  print_table();            break;
		case 'l': case 'L':  cmd_types();              break;
		case 'w': case 'W':  cmd_write();              break;
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
