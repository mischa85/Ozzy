# ==========================================
#  Ozzy USB Audio Driver Makefile
# ==========================================

# Module Name
MODULE_NAME := snd-usb-ozzy

# Kernel Build Directory (defaults to current running kernel)
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# ------------------------------------------
#  Kbuild Definitions
# ------------------------------------------

# The target module
obj-m := $(MODULE_NAME).o

# Linux's side (ozzy.h): the USB driver, the engine's bus on URBs, ALSA PCM, rawmidi and
# controls. The device protocol is the shared engine (engine/, as in the macOS kext and the
# Windows driver) on common/ (protocol, codecs).
$(MODULE_NAME)-y := core.o \
                    bus.o \
                    pcm.o \
                    midi.o \
                    controls.o \
                    ../common/devices/ploytec/ploytec_codec.o \
                    ../common/devices/ploytec/ploytec_handshake.o \
                    ../common/usb_midi.o \
                    ../engine/core/ozzy_engine.o \
                    ../engine/core/ozzy_isoc_output.o \
                    ../engine/devices/ploytec/ploytec_engine.o \
                    ../engine/devices/ploytec/ploytec_io.o \
                    ../engine/devices/ploytec/ploytec_isoc_engine.o \
                    ../engine/devices/usbmidi/usb_midi_engine.o

# Devices to leave out (keys from common/ozzy_devices.def), e.g. make OZZY_DISABLE="ReloopDJ2".
# By default the ones mainline drives: Tascam US-144 (snd-usb-us122l, us144mkii) and US-144MKII
# (us144mkii); make OZZY_DISABLE= builds them in.
OZZY_DISABLE ?= TascamUS144 TascamUS144MKII
ccflags-y += $(addprefix -DOZZY_DISABLE_,$(OZZY_DISABLE))

# Commit the module is built from: logged at probe, shown by `modinfo -F version`
# (common/ozzy_version.h). git describe only where the checkout is Ozzy's own, not a repository
# the sources sit in; else the commit git archive / a GitHub ZIP wrote into ozzy_version.txt.
OZZY_ROOT := $(src)/..
OZZY_GIT_VERSION := $(shell \
	if [ "$$(git -C "$(OZZY_ROOT)" rev-parse --show-toplevel 2>/dev/null)" = "$$(cd "$(OZZY_ROOT)" && pwd -P)" ]; then \
		git -C "$(OZZY_ROOT)" describe --always --dirty; \
	else \
		grep -sv Format "$(OZZY_ROOT)/common/ozzy_version.txt" || echo unknown; \
	fi)
CFLAGS_core.o := -DOZZY_GIT_VERSION='"$(OZZY_GIT_VERSION)"'

# A new device family is an engine (engine/devices/) and its common/ code: add their objects
# above.

# ------------------------------------------
#  Targets
# ------------------------------------------

all:
	@echo "Building $(MODULE_NAME)..."
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	@echo "Cleaning..."
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

# Standard install (installs to /lib/modules/$(uname -r)/extra/)
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: all clean modules_install
