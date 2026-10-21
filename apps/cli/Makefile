# ozzy-cli: the console tool on the shared control core and this platform's backend (control/)

CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wextra   # gnu: POSIX (realpath) and alsa-lib on glibc

UNAME := $(shell uname -s)
ifdef DEMO
# make DEMO=1: made-up devices (control/backend_demo.c), no driver needed
BACKEND := ../control/backend_demo.c
LDLIBS  += -pthread
else ifeq ($(UNAME),Darwin)
BACKEND := ../control/backend_macos.c
CFLAGS  += -mmacosx-version-min=$(or $(MACOSX_DEPLOYMENT_TARGET),10.11)
LDLIBS  += -framework CoreAudio -framework CoreFoundation
else ifeq ($(UNAME),Linux)
BACKEND := ../control/backend_linux.c
LDLIBS  += -lasound -pthread   # alsa-lib (Debian/Ubuntu: libasound2-dev, Fedora: alsa-lib-devel, Arch: alsa-lib)
else
$(error ozzy-cli has no backend for $(UNAME); on Windows build it with CMakeLists.txt)
endif

SRCS := ozzy_cli.c ../control/ozzy_control_core.c $(BACKEND)

ozzy-cli: $(SRCS) ../control/ozzy_control.h ../../engine/shared/ozzy_info.h ../../common/ozzy_settings.h ../../common/ozzy_route.h ../../common/ozzy_quirks.h ../../common/ozzy_devices.h ../../common/ozzy_devices.def
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

clean:
	rm -f ozzy-cli

.PHONY: clean
