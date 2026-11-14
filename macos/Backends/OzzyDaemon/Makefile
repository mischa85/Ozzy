# OzzyDaemon, the userspace driver (ozzy_daemon.c). install.command builds it for option 3:
#   make [GIT_VERSION=...] [MIN_MACOS=10.11] [ARCHS="arm64 x86_64"] [DISABLE_FLAGS=...]
# The engine builds as the engine side of the shared memory in user space (OZZY_USER_ENGINE),
# logging to the unified log (OZZY_MACOS_DAEMON, engine/shared/ozzy_log.h).

OZZY := ../../..
BUILD ?= build
GIT_VERSION ?= $(shell git -C $(OZZY) describe --always --dirty 2>/dev/null || echo unknown)
MIN_MACOS ?= 10.11
ARCHS ?= $(shell uname -m)

SOURCES := \
	ozzy_daemon.c \
	daemon_device.c \
	daemon_bus.c \
	daemon_server.c \
	daemon_log.c \
	$(OZZY)/engine/core/ozzy_engine.c \
	$(OZZY)/engine/core/ozzy_isoc_output.c \
	$(OZZY)/engine/core/ozzy_isoc_input.c \
	$(OZZY)/engine/devices/ploytec/ploytec_engine.c \
	$(OZZY)/engine/devices/ploytec/ploytec_io.c \
	$(OZZY)/engine/devices/ploytec/ploytec_isoc_engine.c \
	$(OZZY)/engine/devices/ploytec/ploytec_us3xx_engine.c \
	$(OZZY)/engine/devices/usbmidi/usb_midi_engine.c \
	$(OZZY)/common/devices/ploytec/ploytec_handshake.c \
	$(OZZY)/common/devices/ploytec/ploytec_codec.c \
	$(OZZY)/common/usb_midi.c

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	-mmacosx-version-min=$(MIN_MACOS) $(foreach a,$(ARCHS),-arch $(a)) \
	-DOZZY_USER_ENGINE -DOZZY_MACOS_DAEMON -DOZZY_GIT_VERSION=\"$(GIT_VERSION)\" $(DISABLE_FLAGS)
LDFLAGS := -mmacosx-version-min=$(MIN_MACOS) $(foreach a,$(ARCHS),-arch $(a)) \
	-framework IOKit -framework CoreFoundation

OBJECTS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SOURCES)))
vpath %.c $(sort $(dir $(SOURCES)))

$(BUILD)/OzzyDaemon: $(OBJECTS)
	clang $(LDFLAGS) $^ -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	clang $(CFLAGS) -MMD -c $< -o $@

$(BUILD):
	mkdir -p $@

# The build's version (OZZY_GIT_VERSION) is in these: compiled every time, so it is never stale
$(BUILD)/ozzy_daemon.o $(BUILD)/daemon_device.o: FORCE
FORCE:

clean:
	rm -rf $(BUILD)

.PHONY: clean FORCE
-include $(OBJECTS:.o=.d)
