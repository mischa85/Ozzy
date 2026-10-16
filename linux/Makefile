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

# Source files: Core + PCM + MIDI + Device families + Shared codecs
$(MODULE_NAME)-y := ozzy_core.o \
                    ozzy_pcm.o \
                    ozzy_midi.o \
                    devices/ploytec.o \
                    ../common/devices/ploytec/ploytec_codec.o \
                    ../common/devices/ploytec/ploytec_handshake.o

# Devices to leave out (keys from common/ozzy_devices.def), e.g. make OZZY_DISABLE="ReloopDJ2"
ccflags-y += $(addprefix -DOZZY_DISABLE_,$(OZZY_DISABLE))

# Commit the module is built from: logged at probe, shown by `modinfo -F version`
OZZY_GIT_VERSION := $(shell git -C $(src) describe --always --dirty 2>/dev/null || echo unknown)
CFLAGS_ozzy_core.o := -DOZZY_GIT_VERSION='"$(OZZY_GIT_VERSION)"'

# To add a new device family, append:
#   devices/newdevice.o
#   ../common/devices/newdevice/newdevice_codec.o  (if shared codec)

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
