# Master Chief on the PS2: make (needs PS2DEV, PS2SDK and gsKit from the ps2dev toolchain)
EE_BIN = masterchief.elf
EE_OBJS = src/main.o src/assets.o
EE_INCS += -I$(PS2DEV)/gsKit/include
EE_LDFLAGS += -L$(PS2DEV)/gsKit/lib
EE_LIBS = -lgskit -ldmakit -lm -lc -lkernel
EE_CFLAGS += -O2 -std=gnu99 $(EXTRA_CFLAGS)

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
