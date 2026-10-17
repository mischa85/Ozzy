# ozzyctl: the core plus this platform's backend (ozzyctl.h)

CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra   # gnu: POSIX (usleep, realpath) and alsa-lib on glibc

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
BACKEND := backend_macos.c
LDLIBS  += -framework CoreAudio -framework CoreFoundation
else ifeq ($(UNAME),Linux)
BACKEND := backend_linux.c
LDLIBS  += -lasound    # alsa-lib (Debian/Ubuntu: libasound2-dev, Fedora: alsa-lib-devel, Arch: alsa-lib)
else
$(error ozzyctl has no backend for $(UNAME) yet)
endif

ozzyctl: ozzyctl.c $(BACKEND) ozzyctl.h ../common/ozzy_settings.h ../common/ozzy_route.h ../common/ozzy_quirks.h ../common/ozzy_devices.h ../common/ozzy_devices.def
	$(CC) $(CFLAGS) -o $@ ozzyctl.c $(BACKEND) $(LDLIBS)

clean:
	rm -f ozzyctl

.PHONY: clean
