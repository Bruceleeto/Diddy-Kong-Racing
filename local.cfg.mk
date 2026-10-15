# The Dreamcast port's project settings for enDjinn's build system
# (third_party/enDjinn/base_link.mk), which Makefile.dc includes: where things
# go, and how the port is compiled and linked. enDjinn reads this file right
# after its own cfg.mk; everything here overrides it. Makefile.dc sets the
# build options used below (FSAA, GFX_PROFILE, ...) before it includes enDjinn.
# The N64 and Linux builds do not use it.

ENJ_BASENAME := dkracing
ENJ_BINDIR := bin
# Makefile.dc's BUILD, so the generated headers and the objects stay together.
ENJ_BUILDDIR := $(BUILD)
ENJ_ROMDIR := cdrom
# enDjinn compiles every .c below this directory: the Dreamcast port layer.
ENJ_CODEDIR := ./dreamcast
# DKR converts its own assets (Makefile.dc); enDjinn's converters stay idle.
ENJ_TEXTURE_SRC_DIR := ./dreamcast/enj_assets/texture
ENJ_SOUNDFX_SRC_DIR := ./dreamcast/enj_assets/sfx
ENJ_FONTS_SRC_DIR := ./dreamcast/enj_assets/fonts
ENJ_CBASEPATH := $(DKR_ASSET_MOUNT)/
# enDjinn's coordinates follow DKR's FSAA; its quick font draws the frame rate.
ENJ_FSAA := $(FSAA)
ENJ_INJECT_QFONT := 1
OPTLEVEL := 3
# The old optimisation knob, OPT=-O2 and the like, still works.
ifdef OPT
OPTLEVEL := $(patsubst -O%,%,$(OPT))
endif

# --- Compiling ---------------------------------------------------------------
# enDjinn's compile line is: $(ENJ_CC) $(ENJ_INCLUDES) $(ENJ_CFLAGS) $(DEFINES)
# plus dependency flags; its defines.mk appends -g and -O$(OPTLEVEL) to DEFINES.
DEFINES := \
	-DTARGET_PC \
	-DTARGET_DC \
	-D_LANGUAGE_C \
	-D_FINALROM \
	-DNDEBUG \
	-DF3DDKR_GBI \
	-DVERSION_US_V77 \
	-DAVOID_UB \
	-DNON_MATCHING \
	-DBUILD_VERSION=VERSION_G \
	-DBUILD_VERSION_STRING=\"2.0G\" \
	-D_MIPS_SZLONG=32 \
	-DPC_MAIN_POOL_BYTES='(3 * 1024 * 1024)' \
	-DGBI_FLOAT_MTX \
	-DAUDIO_NO_REVERB

DEFINES += -DDKR_AICAFLOW -DDKR_AICAFLOW_BENCHMARK=$(AICAFLOW_BENCHMARK) -DDKR_AICAFLOW_TRACE_SOUND=$(AICAFLOW_TRACE_SOUND) -DDKR_GFX_PROFILE=$(GFX_PROFILE) -DDKR_GFX_PROFILE_SKIP=$(GFX_PROFILE_SKIP) -DDKR_AUTO_EXIT_SECONDS=$(AUTO_EXIT_SECONDS) -DPC_LOGIC_UPDATE_RATE=$(FRAME_FIELDS) -DDKR_FSAA=$(FSAA) -DDKR_CUTOUT_1555=$(CUTOUT_1555) -DDKR_TWIDDLE=$(TWIDDLE) -DDKR_DEBUG_VIEW=$(DEBUG_VIEW) -DDKR_DEMO_LEVEL=$(DEMO_LEVEL) -DDKR_TEX_TRACE=$(TEX_TRACE) -DDKR_SKIP_WATER=$(SKIP_WATER) -DDKR_SKIP_PARTICLES=$(SKIP_PARTICLES) -DDKR_FSAA_PARTICLE_THIN=$(FSAA_PARTICLE_THIN) -DDKR_FSAA_PARTICLE_NEAR=$(FSAA_PARTICLE_NEAR) -DDKR_FSAA_PARTICLE_NEAR_FADE=$(FSAA_PARTICLE_NEAR_FADE) -DDKR_FSAA_PARTICLE_NEAR_CULL=$(FSAA_PARTICLE_NEAR_CULL) -DDKR_LEVEL_CULL=$(LEVEL_CULL) -DDKR_VBL_PACER=$(VBL_PACER)
ifeq ($(EEPROM_PRESET_100),1)
DEFINES += -DEEPROM_PRESET_100
endif

ENJ_INCLUDES := \
	-I$(BUILD) \
	-I . \
	-I include \
	-I include/PR \
	-I include/sys \
	-I assets \
	-I src \
	-I src/hasm \
	-I libultra \
	-I libultra/src/gu \
	-I libultra/src/io \
	-I libultra/src/sc \
	-I libultra/src/audio \
	-I libultra/src/os \
	-I$(AICAFLOW_ROOT)/driver/include \
	-I$(AICAFLOW_ROOT)/driver/format/include \
	-I$(AICAFLOW_ROOT)/driver/sh4/include \
	-I$(BUILD)/aicaflow \
	-I$(ENJDIR)include \
	-I$(KOS_BASE)/utils

ENJ_CFLAGS := \
	-std=gnu23 \
	-fno-common \
	-fno-strict-aliasing \
	-Wno-int-conversion \
	-funsigned-char \
	-fwrapv \
	-ffp-contract=fast \
	-fsingle-precision-constant \
	-mfsrra \
	-mfsca \
	-flto \
	-DSHZ_BACKEND=$(SHZ_BACKEND) \
	$(FAST_MATH)

# --- Linking -----------------------------------------------------------------
# LTO runs at link time, so the link gets the optimisation and code flags too.
# One LTO partition: GCC optimises the whole program at once, so inlining and
# code layout do not depend on the order of the objects. With the default
# partitioning, enDjinn's object order put the display-list code elsewhere,
# inlined differently, and took 1.5x the frames with over 11 ms of DL time;
# one partition took a third of them.
ENJ_LDLIBS := $(AICAFLOW_ROOT)/driver/sh4/libaicaflow_host.a -lsh4zam -lm
ENJ_LDFLAGS = -O$(OPTLEVEL) $(filter -f% -m%,$(ENJ_CFLAGS)) -flto-partition=one \
	-Wl,-Map,$(ENJ_BINDIR)/$(ENJ_BASENAME).map

# --- Sources -----------------------------------------------------------------
# The game's sources. enDjinn adds every .c in dreamcast/ (ENJ_CODEDIR) and
# its own runtime.
DKR_GAME_SOURCES := \
	src/asset_loading.c \
	src/audio.c \
	src/audiomgr.c \
	src/audiosfx.c \
	src/audio_spatial.c \
	src/borders.c \
	src/camera.c \
	src/fade_transition.c \
	src/font.c \
	src/game.c \
	src/game_text.c \
	src/game_ui.c \
	src/get_stack_pointer.c \
	src/gzip.c \
	src/joypad.c \
	src/lights.c \
	src/main.c \
	src/memory.c \
	src/menu.c \
	src/object_functions.c \
	src/object_models.c \
	src/objects.c \
	src/os_yield_data.c \
	src/particles.c \
	src/printf.c \
	src/racer.c \
	src/rcp_dkr.c \
	src/save_data.c \
	src/screen_asset.c \
	src/set_rsp_segment.c \
	src/textures_sprites.c \
	src/thread0_epc.c \
	src/thread30_bgload.c \
	src/thread3_main.c \
	src/tracks.c \
	src/vehicle_bluey.c \
	src/vehicle_bubbler.c \
	src/vehicle_rocket.c \
	src/vehicle_smokey.c \
	src/vehicle_tricky.c \
	src/vehicle_wizpig.c \
	src/video.c \
	src/waves.c \
	src/weather.c \
	src/hasm/collision.c \
	src/hasm/math_util.c \
	src/hasm/obj_animate.c \
	src/hasm/obj_shade_fast.c


DKR_LIBULTRA_SOURCES := \
	libultra/src/audio/bnkf.c \
	libultra/src/audio/cents2ratio.c \
	libultra/src/audio/copy.c \
	libultra/src/audio/cseq.c \
	libultra/src/audio/cspgetchlvol.c \
	libultra/src/audio/cspgetstate.c \
	libultra/src/audio/cspgettempo.c \
	libultra/src/audio/cspplay.c \
	libultra/src/audio/cspsetbank.c \
	libultra/src/audio/cspsetchlvol.c \
	libultra/src/audio/cspsetpan.c \
	libultra/src/audio/cspsetseq.c \
	libultra/src/audio/cspsettempo.c \
	libultra/src/audio/cspsetvol.c \
	libultra/src/audio/cspstop.c \
	libultra/src/audio/env.c \
	libultra/src/audio/event.c \
	libultra/src/audio/filter.c \
	libultra/src/audio/heapalloc.c \
	libultra/src/audio/heapinit.c \
	libultra/src/audio/load.c \
	libultra/src/audio/mainbus.c \
	libultra/src/audio/mips1/auxbus.c \
	libultra/src/audio/mips1/cspgetfadein.c \
	libultra/src/audio/mips1/csplayer.c \
	libultra/src/audio/mips1/cspsetfadein.c \
	libultra/src/audio/mips1/drvrnew.c \
	libultra/src/audio/mips1/reverb.c \
	libultra/src/audio/mips1/seqchannel.c \
	libultra/src/audio/mips1/seqplayer.c \
	libultra/src/audio/mips1/synallocfx.c \
	libultra/src/audio/mips1/synsetpan.c \
	libultra/src/audio/mips1/synstartvoiceparam.c \
	libultra/src/audio/mips1/synthesizer.c \
	libultra/src/audio/mips1/unknown_064690.c \
	libultra/src/audio/resample.c \
	libultra/src/audio/save.c \
	libultra/src/audio/seqpgetfxmix.c \
	libultra/src/audio/sl.c \
	libultra/src/audio/synaddplayer.c \
	libultra/src/audio/synallocvoice.c \
	libultra/src/audio/syndelete.c \
	libultra/src/audio/synfreevoice.c \
	libultra/src/audio/synsetfxmix.c \
	libultra/src/audio/synsetpitch.c \
	libultra/src/audio/synsetpriority.c \
	libultra/src/audio/synsetvol.c \
	libultra/src/audio/synstartvoice.c \
	libultra/src/audio/synstopvoice.c \
	libultra/src/debug/assert.c \
	libultra/src/debug/kdebugserver.c \
	libultra/src/gu/mtxutil.c \
	libultra/src/gu/perspective.c \
	libultra/src/io/crc.c \
	libultra/src/io/devmgr.c \
	libultra/src/io/dp.c \
	libultra/src/io/dpgetstat.c \
	libultra/src/io/dpsetnextbuf.c \
	libultra/src/io/dpsetstat.c \
	libultra/src/io/epirawdma.c \
	libultra/src/io/epirawwrite.c \
	libultra/src/io/leointerrupt.c \
	libultra/src/io/piacs.c \
	libultra/src/io/pidma.c \
	libultra/src/io/pigetcmdq.c \
	libultra/src/io/pimgr.c \
	libultra/src/io/pirawdma.c \
	libultra/src/io/pirawread.c \
	libultra/src/io/si.c \
	libultra/src/io/siacs.c \
	libultra/src/io/sirawread.c \
	libultra/src/io/sirawwrite.c \
	libultra/src/io/sp.c \
	libultra/src/io/spgetstat.c \
	libultra/src/io/sprawdma.c \
	libultra/src/io/spsetpc.c \
	libultra/src/io/spsetstat.c \
	libultra/src/io/sptask.c \
	libultra/src/io/sptaskyield.c \
	libultra/src/io/sptaskyielded.c \
	libultra/src/io/vi.c \
	libultra/src/io/viblack.c \
	libultra/src/io/vigetcurrcontext.c \
	libultra/src/io/vigetcurrframebuf.c \
	libultra/src/io/vigetnextframebuf.c \
	libultra/src/io/vimgr.c \
	libultra/src/io/visetevent.c \
	libultra/src/io/visetmode.c \
	libultra/src/io/visetspecial.c \
	libultra/src/io/viswapbuf.c \
	libultra/src/io/viswapcontext.c \
	libultra/src/io/vitbl.c \
	libultra/src/libc/ldiv.c \
	libultra/src/libc/ll.c \
	libultra/src/libc/llcvt.c \
	libultra/src/libc/rmonPrintf.c \
	libultra/src/libc/xldtob.c \
	libultra/src/libc/xlitob.c \
	libultra/src/libc/xprintf.c \
	libultra/src/os/createmesgqueue.c \
	libultra/src/os/createthread.c \
	libultra/src/os/destroythread.c \
	libultra/src/os/getactivequeue.c \
	libultra/src/os/getthreadpri.c \
	libultra/src/os/gettime.c \
	libultra/src/os/initialize.c \
	libultra/src/os/jammesg.c \
	libultra/src/os/recvmesg.c \
	libultra/src/os/resetglobalintmask.c \
	libultra/src/os/sendmesg.c \
	libultra/src/os/seteventmesg.c \
	libultra/src/os/sethwinterrupt.c \
	libultra/src/os/setthreadpri.c \
	libultra/src/os/settime.c \
	libultra/src/os/settimer.c \
	libultra/src/os/startthread.c \
	libultra/src/os/stopthread.c \
	libultra/src/os/thread.c \
	libultra/src/os/timerintr.c \
	libultra/src/os/virtualtophysical.c \
	libultra/src/os/yieldthread.c \
	libultra/src/sc/sched.c

OBJS := $(addprefix $(ENJ_BUILDDIR)/,$(DKR_GAME_SOURCES:.c=.o) $(DKR_LIBULTRA_SOURCES:.c=.o))
