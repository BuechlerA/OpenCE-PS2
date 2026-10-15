# Master Chief on the PS2: make (needs PS2DEV, PS2SDK and gsKit from the ps2dev toolchain)
# make NO_MAP=1 draws the Chief on the old ground grid, without the map's data; NO_CHIEF_VU1=1 keeps his drawing on the EE
EE_BIN = masterchief.elf
EE_OBJS = src/main.o src/assets.o
ifdef NO_MAP
override EXTRA_CFLAGS += -DNO_MAP
else
EE_OBJS += src/map_assets.o
# the map scene is first person (src/fp_controller.c reads the DualShock through src/pad.c, src/fp_demo.c walks it when no hand is on the
# pad): make FP_NO_DEMO=1 leaves the player standing for the pad alone, ORBIT_CAMERA=1 keeps the camera orbiting the Chief (builds for
# the stills and the profile do)
EE_OBJS += src/fp_controller.o src/fp_demo.o src/fp_weapon.o src/fp_view.o src/fp_hud.o src/hud_assets.o src/weapon_assets.o src/fpm_assets.o src/pad.o
ifdef FP_NO_DEMO
override EXTRA_CFLAGS += -DFP_NO_DEMO
endif
ifdef ORBIT_CAMERA
override EXTRA_CFLAGS += -DORBIT_CAMERA
endif
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
EE_LIBS = -lgskit -ldmakit $(if $(CHIEF_VU1),-ldma) $(if $(NO_MAP),,-lpad) -lm -lc -lkernel
EE_CFLAGS += -O3 -ffast-math -funroll-loops -std=gnu99 $(EXTRA_CFLAGS)

all: $(EE_BIN)

%.o: %.vsm
	$(EE_DVP) $< -o $@

clean:
	rm -f $(EE_BIN) $(EE_OBJS) src/chief_vu1.o src/fp_controller.o src/fp_demo.o src/fp_weapon.o src/fp_view.o src/fp_hud.o src/hud_assets.o src/weapon_assets.o src/fpm_assets.o src/pad.o

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
