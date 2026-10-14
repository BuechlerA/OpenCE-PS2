# EE micro-benchmark: make (needs PS2DEV and PS2SDK from the ps2dev toolchain)
EE_BIN = bench.elf
EE_OBJS = main.o
EE_LIBS = -lm -lc -lkernel
EE_CFLAGS += -O2 -std=gnu99

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
