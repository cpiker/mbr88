# MBR88
An OS independent 8088 op-code clean MBR for dual booting ELKS, FreeDOS or other
OS up through many modern operating systems. Patching tool currently cross compiles 
natively for linux and cross compiles for [ELKS](https://github.com/ghaerr/elks) via
[ia16-gcc](https://github.com/tkchia/gcc-ia16) and [FreeDOS](https://github.com/FDOS) 
via [OpenWatcom](https://github.com/open-watcom).  MBR assembly is provided in
NASM native syntax.

Safe for ELKS, but *don't use this yet on FreeDOS yet, currently still testing the
Watcom build of mbrpatch.*

The MBR reserves space within it's tiny 512 byte area for volume names to display
in the boot menu.  MBR88 always presents a boot menu and waits for user input.  Since 
it never auto-boots it's good for desktops, but not suitable for servers.  Only
partitions marked with the 0x80 bootable flag appear in the menu but up to *four*
partitions may be simultaneously marked as bootable.  The boot menu also allows for
booting from a floppy even if it's installed on the first hardrive or or flash card.

![Screenshot ELKS/FreeDOS dualboot](screenshot.png)

Written by C. Piker and Claude (Anthropic).  Verified on a Leading Edge Model D with
two floppy drives and a SD card reader with XTIDE installed. More hardware and OS
verification is in the works.

## Acknowledgement
Thanks to osdev.org for the reference material and insights that informed the
design of this boot record, especially the creators of [this page](https://wiki.osdev.org/MBR_(x86)).
