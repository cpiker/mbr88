# Source your ELKS environment before building this.  For example:
#
# cd elks
# source env.sh
# cd mbr88
# make

.PHONY: gas nasm

# Should probably use "make gas" when building in elks environment
gas:build build/mbr88_gas.bin build/mbr_patch

# Here's a build for native systems with nasm & gcc
nasm:build build/mbr88_nasm.bin build/mbr_patch_native

build: 
	@if [ ! -e "$(BD)" ]; then mkdir build; fi

# Make both
all:build \
 build/mbr88_nasm.bin \
 build/mbr88_gas.bin \
 build/mbr_patch \
 build/mbr_patch_native


build/mbr88_nasm.bin:src/mbr88_nasm.asm
	nasm -f bin $< -o $@
	
build/mbr88_gas.o:src/mbr88_gas.s
	ia16-elf-as -o $@ $<

build/mbr88_gas.bin:build/mbr88_gas.o
	ia16-elf-ld -Ttext=0x7C00 --oformat=binary -o $@ $<

build/mbr_patch:src/mbr_patch.c
	ia16-elf-gcc -melks -Os -o $@ $<

build/mbr_patch_native:src/mbr_patch.c
	gcc -std=c99 -Wall -o $@ $<

clean:
	-rm build/*.o 

distclean:
	-rm -r build