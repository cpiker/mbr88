# A make file for MBR88, a multi-boot MBR record and MBR editor.
# This makefile presumes cross compilation
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

BN:=build/native
BE:=build/elks
BD:=build/freedos

native: $(BN) $(BN)/mbr88.bin $(BN)/mk_head $(BN)/mbr88.h $(BN)/mbrpatch

#elks: $(BE) $(BE)/mbr88_g.bin $(BE)/mk_head $(BE)/mbr88.h $(BE)/mbrpatch
elks: $(BE) $(BE)/mbr88.bin $(BE)/mk_head $(BE)/mbr88.h $(BE)/mbrpatch

freedos: $(BD) $(BD)/mbr88.bin $(BD)/mk_head $(BD)/mbr88.h $(BD)/mbrpatch.com

# Used to insure the template MBR binaries emitted by NASM
# and GAS are identical.  Requireds the ELKS environment or
# some other source of ia16-elf-as & ia16-elf-ld 
check_equal:$(BN)/mbr88.bin $(BE)/mbr88_g.bin
	N=$$(md5sum $(BN)/mbr88.bin | awk '{print $$1}') && G=$$(md5sum $(BE)/mbr88_g.bin | awk '{print $$1}') && [ "$$N" = "$$G" ]


# Native builds ##############################################################
$(BN):
	@if [ ! -e "$@" ]; then mkdir -p $@; fi

$(BN)/mbr88.bin:src/mbr88.asm
	nasm -f bin $< -o $@

$(BN)/mk_head:src/mk_head.c
	gcc -Wall $< -o $@ 

$(BN)/mbr88.h:$(BN)/mk_head $(BN)/mbr88.bin
	./$(BN)/mk_head $(BN)/mbr88.bin $(BN)/mbr88.h

# Strip is optional, just trying to save space on disk
$(BN)/mbrpatch:src/mbrpatch.c $(BN)/mbr88.h
	gcc -Wall -I $(BN) $< -o $@
	strip $@  

# ELKS Cross-compile #########################################################
$(BE):
	@if [ ! -e "$@" ]; then mkdir -p $@; fi

$(BE)/mbr88.bin:src/mbr88.asm
	nasm -f bin -o $@ $<	

# This is built native since it's an intermediary file
$(BE)/mk_head:src/mk_head.c
	gcc -Wall $< -o $@

$(BE)/mbr88.h:$(BE)/mk_head $(BE)/mbr88.bin
	./$< $(BE)/mbr88.bin $(BE)/mbr88.h

$(BE)/mbrpatch:src/mbrpatch.c $(BE)/mbr88.h
	ia16-elf-gcc -I $(BE) -melks -Os $< -o $@

# Old GAS compile commands, save here for now, drop later
# gas: $(BE)/mbr88_g.bin
#
#$(BE)/mbr88_g.o:src/mbr88_g.s
#	ia16-elf-as -o $@ $<
#
#$(BE)/mbr88_g.bin:$(BE)/mbr88_g.o
#	ia16-elf-ld -Ttext=0x7C00 --oformat=binary -o $@ $<


# FreeDOS cross-compile ######################################################
# (work needed here)

$(BD):
	@if [ ! -e "$@" ]; then mkdir -p $@; fi

$(BD)/mbr88.bin:src/mbr88.asm
	nasm -f bin $< -o $@

$(BD)/mk_head:src/mk_head.c
	wcl $< -o $@ 

$(BD)/mbr88.h:$(BD)/mk_head $(BD)/mbr88.bin
	./$(BD)/mk_head $(BD)/mbr88.bin $(BD)/mbr88.h

$(BD)/mbrpatch:src/mbrpatch.c $(BD)/mbr88.h
	wcl -I$(BD) $< -o $@ 

# Helpers ####################################################################
clean:
	-rm build/*/*.o build/*/mk_head build/*/mbr88.h

distclean:
	-rm -r build
