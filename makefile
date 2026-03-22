# A make file for MBR88, a multi-boot MBR record and MBR editor.
#
# 1. To generate native Linux MBR patching tools just run:
#
#      make
#
# 2. To generate ELKS binaries:
#  
#    Source your ELKS environment then return here:  For example:
#      
#      cd ../elks
#      source env.sh
#      cd ../mbr88
#   
#    Then run make elks:
#
#      make elks
#
# 3. To generate FreeDOS binaries: (todo)
#  
#    a.) ??? 
#    b.) Profit

.PHONY: all native elks freedos

# Here's a build for native systems with nasm & gcc
native:build \
 build/mbr88_nasm.bin \
 build/mbr88_tplt.h \
 build/mbr_patch_native \

# Build the tools for an elks target
elks:build \
 build/mbr88_tplt.h \
 build/mbr_patch

# Build for a freedos target, asperational at this point
freedos:build \
 build/mbr88_tplt.h \
 build/mbr_patch.exe 


# Check everything, and insure gas and nasm versions are
# give identical output
all: build_mbr88_gas.bin native elks freedos

build: 
	@if [ ! -e "$(BD)" ]; then mkdir build; fi

build/mbr88_nasm.bin:src/mbr88_nasm.asm
	nasm -f bin $< -o $@

build/mbr88_gas.bin:build/mbr88_gas.o
	ia16-elf-ld -Ttext=0x7C00 --oformat=binary -o $@ $<


# Assume the template is only regenerated on an native system
# otherwise the one checked into git is fine.
build/mbr88_tplt.h:build/mbr88_nasm.bin
	gcc -std=c99 -Wall -o mbr88_tplt
	build/mbr88_tplt $< $@
	
		
build/mbr88_gas.o:src/mbr88_gas.s
	ia16-elf-as -o $@ $<


build/mbr_patch:src/mbr_patch.c
	ia16-elf-gcc -melks -Os -o $@ $<

build/mbr_patch_native:src/mbr_patch.c
	gcc -std=c99 -Wall -o $@ $<

clean:
	-rm build/*.o 

distclean:
	-rm -r build
