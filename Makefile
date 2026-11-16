# Usage:
#   make                              # TDLib in system paths
#   make TDLIB_PREFIX=/opt/td         # TDLib installed to a custom prefix
CC ?= gcc
PKGS = gtk4 json-glib-1.0 libqrencode

CFLAGS  += -Wall -Wextra -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -ltdjson

ifdef TDLIB_PREFIX
CFLAGS  += -I$(TDLIB_PREFIX)/include
LDFLAGS += -L$(TDLIB_PREFIX)/lib -Wl,-rpath,$(TDLIB_PREFIX)/lib
endif

tgtk: main.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f tgtk

.PHONY: clean
