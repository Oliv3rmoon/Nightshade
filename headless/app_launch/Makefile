# Nightshade - The launch payload (main.c), built as a payload for the console's loader the way
# the self-update helper is (headless/self_update_helper/Makefile), and linked as Mihawk's
# PS5 RetroArch links its forwarder's (tools/build-forwarder.sh).
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
include /opt/ps5-payload-sdk/toolchain/prospero.mk
endif

ROOT := ../..
OUTPUT ?= $(ROOT)/build/app-launch/app-launch.elf
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
LDFLAGS := -Wl,--build-id=none
LIBS := -lSceSystemService -lSceUserService

.PHONY: all clean

all: $(OUTPUT)

$(OUTPUT): main.c
	@mkdir -p -- $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f -- $(OUTPUT)
