# MBR88 OS Compatibility Notes

This document surveys operating systems that a person might reasonably install on
IBM XT-class (8088/8086) hardware, with notes on how each interacts with the
MBR88 boot record and the standard MBR partition table structure.

The focus is on coexistence: can multiple operating systems share a single
partitioned hard disk and be independently selected from the mbr88 boot menu?


## The Bootable Flag

Each partition table entry contains a one-byte status field at offset 0 of the
16-byte entry.  The conventional values are:

| Value  | Meaning                        |
|--------|--------------------------------|
| `0x80` | Bootable / active              |
| `0x00` | Not bootable / inactive        |

MBR88 **only displays and boots partitions marked `0x80`**.  This is intentional:
it prevents accidentally handing control to a data partition whose VBR contains
only a "not a bootable disk" stub message.

**Unlike conventional MBR loaders, MBR88 permits more than one partition to be
marked `0x80` at the same time.**  All bootable partitions appear in the menu and
can be selected freely.  The user sets which partitions are bootable using
`mbrpatch`'s `b` command.

Conventional tools such as DOS `FDISK` enforce a single active partition; they
will clear the flag on all other partitions when they set one active.  After
running such tools you will need to re-run `mbrpatch -p` to restore the flags on
any additional partitions you want visible in the MBR88 menu.


## When to Re-run mbrpatch

Several events will require running `mbrpatch` again after the fact:

- **Installing any OS that writes its own MBR boot code** — Xenix, some versions
  of CP/M-86, and any modern OS installer (Linux, BSD) will overwrite the MBR88
  boot record.  Run `mbrpatch -u` to reinstall MBR88, then `mbrpatch -p` to
  restore labels and bootable flags.

- **Running DOS `FDISK`** — FDISK clears the active flag on all partitions except
  the one it makes active.  Run `mbrpatch -p` afterward to re-set `0x80` on any
  additional partitions you want selectable.

- **Running DOS `FDISK /MBR`** — This command rewrites the MBR boot code with the
  standard DOS single-boot loader, erasing MBR88.  Run `mbrpatch -u` to restore.

- **Adding or repartitioning** — Any time the partition table changes you should
  verify labels and bootable flags with `mbrpatch -p`.


## Operating System Survey

### FreeDOS  (ongoing, open source)

FreeDOS is a free, open-source DOS-compatible OS actively developed and tested on
real 8088/8086 hardware.  It is fully compatible with MBR88 and has been verified
working on the Leading Edge Model D test platform used to develop MBR88.

**Partition types:** `0x01`, `0x04`, `0x06`

**VBR `55 AA` signature:** Present.

**MBR overwrite risk:** `FDISK /MBR` rewrites the MBR.  Same procedure as MS-DOS.

**Bootable-flag behavior:** Same as MS-DOS.  FreeDOS `FDISK` enforces one active
partition; use `mbrpatch -p` to restore multi-boot flags afterward.

---

### ELKS — Embeddable Linux Kernel Subset  (ongoing, open source)

ELKS is a modern open-source project that ports a minimal Linux-like kernel to
8086/8088 hardware.  It is designed with current best practices and full awareness
of the MBR boot convention.

ELKS has been verified working on the Leading Edge Model D test platform used to
develop MBR88, including multi-partition coexistence with FreeDOS.

**Partition type:** `0x80` (MINIX) is conventional for ELKS.

**VBR `55 AA` signature:** Present.

**MBR overwrite risk:** ELKS installation tools may write boot code to the MBR.
Reinstall with `mbrpatch -u` if needed.

**Bootable-flag behavior:** Standard.  Uses `0x80` for the bootable partition.
Multi-boot with DOS and other systems works correctly with MBR88.

---

### MS-DOS / PC-DOS 2.0 – 3.3  (1983 – 1987)

The MBR and the `0x80` active-partition convention were introduced with PC DOS 2.0
specifically for the IBM PC XT.  All versions of MS-DOS and PC-DOS in this range
use a standard FAT VBR with a `55 AA` boot signature and are fully compatible with
MBR88.

Multiple DOS versions (e.g. PC-DOS 3.1 and MS-DOS 3.3) can coexist on separate
FAT partitions.  Each can be marked `0x80` and selected independently from the
MBR88 menu.

**Partition types:** `0x01` (FAT12), `0x04` (FAT16 <32 MB), `0x06` (FAT16 ≥32 MB)

**VBR `55 AA` signature:** Present in all versions from 2.0 onward.

**MBR overwrite risk:** `FDISK /MBR` rewrites the MBR.  Reinstall with
`mbrpatch -u` afterward.

**Bootable-flag behavior:** DOS `FDISK` enforces exactly one active partition.
After using FDISK, run `mbrpatch -p` to re-flag any additional boot partitions.

---

### DR-DOS / Novell DOS  (1988 onward)

Digital Research DR-DOS (later Novell DOS, then Caldera OpenDOS) is a
DOS-compatible OS notable for squeezing LBA, FAT32, and other extensions into
single-sector VBR code while maintaining full 8088 compatibility.  It is
structurally identical to MS-DOS from an MBR perspective.

**Partition types:** `0x01`, `0x04`, `0x06`

**VBR `55 AA` signature:** Present.

**MBR overwrite risk:** `FDISK /MBR` rewrites the MBR.

**Bootable-flag behavior:** Same as MS-DOS.

---

### Windows 1.x / 2.x  (1985 – 1987)

Windows 1.x and 2.x are graphical shells that run on top of MS-DOS; they are not
standalone operating systems.  Booting means booting DOS first and then launching
Windows from within DOS.  There are no independent partition or boot record
requirements beyond those of the underlying DOS version.

**Partition types:** Same as the underlying MS-DOS.

**VBR `55 AA` signature:** Present (inherited from DOS).

**MBR overwrite risk:** None specific to Windows.  The DOS installer carries the
same risk as noted above.

**Bootable-flag behavior:** Same as MS-DOS.  No special considerations.

---

### CP/M-86  (Digital Research, 1982 – 1983)

CP/M-86 is the 8086/8088 port of the classic CP/M operating system.  It predates
the standardized MBR partition scheme and has its own partitioning conventions.

**Partition type:** `0xDB` (CP/M, Concurrent CP/M, Concurrent DOS)

**VBR `55 AA` signature:** Not reliably present.  Early CP/M-86 boot sectors
predate the `55 AA` convention and may not carry the signature.  MBR88 checks for
`55 AA` before jumping to the VBR; a CP/M-86 partition without the signature will
be reported as "No boot record" rather than booting.  Whether a given CP/M-86
installation has the signature depends on the specific version and how it was
installed.

**MBR overwrite risk:** CP/M-86's `HDMAINT.CMD` disk setup tool may write its own
boot code to the MBR.  Reinstall MBR88 with `mbrpatch -w` afterward.

**Bootable-flag behavior:** CP/M-86's installer may set the status byte to a
non-standard value.  Verify with `mbrpatch -r` after installation.

**Coexistence with DOS:** Structurally possible — CP/M-86 uses its own partition
type and the partition table is shared.  In practice, run CP/M-86 installation
first, then reinstall MBR88, then add the DOS partitions.

---

### SCO Xenix  (Microsoft/SCO, 1984 for XT)

SCO Xenix was probably the most widely deployed Unix variant of the mid-1980s.
The 8086/8088 version shipped for the PC XT starting around 1984.  Xenix uses
standard MBR partition table entries and in principle coexists with DOS on the
same disk.

**Partition types:** `0x02` (Xenix root), `0x03` (Xenix /usr)

**VBR `55 AA` signature:** Version-dependent.  Unix systems of this era did not
always carry the `55 AA` signature in the partition boot sector.  As with CP/M-86,
a missing signature will cause MBR88 to report "No boot record."  Testing with
your specific Xenix version and install media is necessary.

**MBR overwrite risk:** High.  Xenix installs its own boot loader into the MBR.
Reinstall MBR88 with `mbrpatch -w` after completing the Xenix installation.

**Bootable-flag behavior:** Xenix may set the status byte differently from the
DOS convention.  Verify and correct with `mbrpatch -p` after installation.

**Coexistence with DOS:** Possible but requires care.  Install Xenix first (it
will partition and write its MBR), then install DOS into a separate partition, then
reinstall MBR88 and set bootable flags on the desired partitions.

---


## Compatibility Summary Table

| Operating System     | VBR `55 AA` | MBR overwrite risk | Bootable flag | Multi-boot with DOS |
|----------------------|-------------|-------------------|---------------|---------------------|
| FreeDOS              | ✓ Yes       | `FDISK /MBR`      | Standard      | ✓ Yes (tested)      |
| ELKS                 | ✓ Yes       | Unsure            | Standard      | ✓ Yes (tested)      |
| MS-DOS / PC-DOS 2–3  | ✓ Yes       | `FDISK /MBR`      | Standard      | ✓ Yes               |
| DR-DOS / Novell DOS  | ✓ Yes       | `FDISK /MBR`      | Standard      | ✓ Yes               |
| Windows 1.x / 2.x   | ✓ Yes       | Via DOS installer | Standard      | ✓ Yes (runs on DOS) |
| CP/M-86              | ⚠ Maybe    | `HDMAINT.CMD`     | Non-standard  | ⚠ Possible          |
| SCO Xenix            | ⚠ Maybe    | Installer         | Non-standard  | ⚠ Possible          |

**Key:**
- ✓ Yes — works reliably, may have been tested on real hardware
- ⚠ Maybe — possible but requires verification and careful installation order


## Recommended Installation Order for Multi-Boot Setups

When mixing OS types, install in this order to minimize re-work:

1. Install any OS that writes its own MBR code last, or be prepared to run
   `mbrpatch -u` after it (Xenix, CP/M-86).
2. Install DOS-family OSes in their desired partitions.
3. Install ELKS in its partition.
4. Run `mbrpatch -u` on the disk to install or restore mbr88 boot code.
5. Run `mbrpatch -p` to set partition labels and mark all desired boot
   partitions with status `0x80`.
6. Test each partition from the mbr88 boot menu.

If a partition shows "No boot record," that OS either does not place `55 AA` at
offset `0x1FE` of its VBR, or the VBR was not installed correctly.  mbr88 will
not chain-load to it in that case — this is a safety measure, not a bug.
