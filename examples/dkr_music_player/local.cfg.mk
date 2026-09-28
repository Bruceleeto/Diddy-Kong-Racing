ENJ_INJECT_QFONT := 1
OPTLEVEL := 2
ENJ_CFLAGS += -Wall -Wextra -Werror

DKR_ROOT := $(abspath ../..)
AICAFLOW_ROOT := $(DKR_ROOT)/third_party/aicaflow
DKR_PYTHON ?= $(shell for candidate in $(DKR_ROOT)/.venv/bin/python3 /opt/homebrew/bin/python3 python3.14 python3.13 python3.12 python3.11 python3.10; do if command -v "$$candidate" >/dev/null 2>&1; then command -v "$$candidate"; break; fi; done)
ENJ_INCLUDES += -I$(AICAFLOW_ROOT)/driver/include -I$(AICAFLOW_ROOT)/driver/sh4/include
ENJ_LDLIBS += $(AICAFLOW_ROOT)/driver/sh4/libaicaflow_host.a
AICA_FIRMWARE := $(ENJ_ROMDIR)/$(ENJ_BASENAME)/aicaflow.drv

.PHONY: dkr-music-player-assets check

dkr-music-player-assets:
	$(MAKE) -C $(DKR_ROOT) -f Makefile.dc aicaflow-music-visuals
	$(DKR_PYTHON) prepare.py $(DKR_ROOT) $(AICAFLOW_ROOT)/tools cdrom/$(ENJ_BASENAME)

include/songs.h: dkr-music-player-assets

assets: dkr-music-player-assets $(AICA_FIRMWARE)
$(ENJ_BUILDDIR)/code/main.o: include/songs.h $(AICAFLOW_ROOT)/examples/music_player/player.c
$(ENJ_BINDIR)/$(ENJ_BASENAME).elf: $(AICAFLOW_ROOT)/driver/sh4/libaicaflow_host.a | $(AICA_FIRMWARE)
$(ENJ_BINDIR)/$(ENJ_BASENAME).cdi: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf $(AICA_FIRMWARE)

$(AICA_FIRMWARE): $(AICAFLOW_ROOT)/driver/arm7/aicaflow.drv
	mkdir -p $(@D)
	cp $< $@

$(AICAFLOW_ROOT)/driver/arm7/aicaflow.drv $(AICAFLOW_ROOT)/driver/sh4/libaicaflow_host.a:
	$(MAKE) -C $(@D)

check: $(ENJ_BINDIR)/$(ENJ_BASENAME).elf dkr-music-player-assets
	$(DKR_PYTHON) prepare.py --verify $(DKR_ROOT) $(AICAFLOW_ROOT)/tools cdrom/$(ENJ_BASENAME)
