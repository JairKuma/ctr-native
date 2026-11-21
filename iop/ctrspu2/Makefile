# ctrspu2.irx - modulo del IOP para mandar el sonido de CTR al chip SPU2.
# Lo compila Makefile.ps2 (make -C iop/ctrspu2) y se incrusta en el ELF.

IOP_BIN = ctrspu2.irx
IOP_OBJS = ctrspu2.o imports.o

all: $(IOP_BIN)

clean:
	rm -rf obj $(IOP_BIN) *.notiopmod.elf *.notiopmod.stripped.elf

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
