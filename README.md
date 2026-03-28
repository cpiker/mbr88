# mbr88
An OS independent 8088 op-code clean MBR for dual booting ELKS, FreeDOS or other
OS up through Windows 98.  Currently cross compiles for ELKS.  FreeDOS via
OpenWatcom is in the works.

*Don't use this yet.  Currently adding support for Windows NT and OS/2 disk
signatures.*

A custom 512-byte MBR boot record for IBM XT-class hardware, plus supporting
tools. Should work all the way through the Windows 95 era, so long as the machine
is booted to real-mode DOS.  Designed for multi-booting FreeDOS, ELKS Linux,
and other OSes on a single partitioned hard drive. Always presents a boot menu
and waits for user input -- never auto-boots. Only partitions marked with the
0x80 bootable flag appear in the menu; multiple partitions may be simultaneously
marked 0x80. Also allows for booting to a floppy even if initally loaded from a 
hard drive or flash card.

![Screenshot ELKS/FreeDOS dualboot](screenshot.png)

Written by Claude Code under heavy interrogation by C. Piker.  Verified on
a Leading Edge Model D with two floppy drives and a four partition hard
disk with both ELKS Linux and FreeDos installed.  More hardware verification
in the works.
