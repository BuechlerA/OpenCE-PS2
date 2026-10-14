# Master Chief on the PS2: make (needs PS2DEV, PS2SDK and gsKit from the ps2dev toolchain)
# make NO_MAP=1 draws the Chief on the old ground grid, without the map's data; NO_CHIEF_VU1=1 keeps his drawing on the EE
EE_BIN = masterchief.elf
EE_OBJS = src/main.o src/assets.o
ifdef NO_MAP
override EXTRA_CFLAGS += -DNO_MAP
else
EE_OBJS += src/map_assets.o
endif
# The map scene draws the Chief's textured passes on VU1 (src/chief_vu1.vsm, assembled with dvp-as, which is in the ps2dev toolchain's
# dvp/bin: put it on PATH); make NO_CHIEF_VU1=1 draws them on the EE, as before
ifndef NO_MAP
ifndef NO_CHIEF_VU1
CHIEF_VU1 = 1
endif
endif
ifdef CHIEF_VU1
override EXTRA_CFLAGS += -DCHIEF_VU1
EE_OBJS += src/chief_vu1.o
EE_DVP = dvp-as
endif
EE_INCS += -I$(PS2DEV)/gsKit/include
EE_LDFLAGS += -L$(PS2DEV)/gsKit/lib
EE_LIBS = -lgskit -ldmakit $(if $(CHIEF_VU1),-ldma) -lm -lc -lkernel
EE_CFLAGS += -O3 -ffast-math -funroll-loops -std=gnu99 $(EXTRA_CFLAGS)

all: $(EE_BIN)

%.o: %.vsm
	$(EE_DVP) $< -o $@

clean:
	rm -f $(EE_BIN) $(EE_OBJS) src/chief_vu1.o

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
