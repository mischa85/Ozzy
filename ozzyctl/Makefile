# ozzyctl: the core plus this platform's backend (ozzyctl.h)

CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
BACKEND := backend_macos.c
LDLIBS  += -framework CoreAudio -framework CoreFoundation
else
$(error ozzyctl has no backend for $(UNAME) yet; on Linux the same settings are ALSA controls: amixer)
endif

ozzyctl: ozzyctl.c $(BACKEND) ozzyctl.h ../common/ozzy_settings.h ../common/ozzy_route.h ../common/ozzy_quirks.h
	$(CC) $(CFLAGS) -o $@ ozzyctl.c $(BACKEND) $(LDLIBS)

clean:
	rm -f ozzyctl

.PHONY: clean
