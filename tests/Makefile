# Host tests for common/ and engine/ (macOS and Linux, any C compiler):
#   make check    build and run them
#   make golden   regenerate golden/ after an intended change (and read the diff)
CC       ?= cc
CFLAGS   ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CPPFLAGS += -DOZZY_USER_ENGINE -I..
BUILD    := build
ENGINE   := ../engine/core/ozzy_engine.c ../engine/core/ozzy_isoc_output.c \
            ../engine/devices/ploytec/ploytec_engine.c ../engine/devices/ploytec/ploytec_io.c \
            ../engine/devices/ploytec/ploytec_isoc_engine.c ../engine/devices/usbmidi/usb_midi_engine.c \
            ../common/devices/ploytec/ploytec_handshake.c ../common/devices/ploytec/ploytec_codec.c \
            ../common/usb_midi.c
TESTS    := test_endpoints test_trace test_routing test_isoc_ring test_memory test_running_status test_layout
HEADERS  := $(wildcard ../engine/*/*.h ../engine/devices/*/*.h ../common/*.h ../common/devices/*/*.h) fake_bus.h check.h

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD):
	mkdir -p $@

$(BUILD)/%: %.c fake_bus.c $(ENGINE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< fake_bus.c $(ENGINE)

check: all
	@./run.sh $(BUILD)

golden: all
	$(BUILD)/test_trace > golden/trace.txt 2>/dev/null
	$(BUILD)/test_layout > golden/layout.txt

clean:
	rm -rf $(BUILD)

.PHONY: all check golden clean
