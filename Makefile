CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

WAY_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAY_LIBS   := $(shell pkg-config --libs wayland-client)

BIN = wl-paper

STB_URL = https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
WLR_URL = https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/raw/master/unstable/wlr-layer-shell-unstable-v1.xml
XDG_URL = https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/main/stable/xdg-shell/xdg-shell.xml

STB_H = stb_image.h
WLR_XML = protocols/wlr-layer-shell-unstable-v1.xml
XDG_XML = protocols/xdg-shell.xml

GEN_WLR_H = wlr-layer-shell-unstable-v1-client-protocol.h
GEN_WLR_C = wlr-layer-shell-unstable-v1-protocol.c
# The layer-shell protocol refers to xdg_popup, so its code needs this object at link time.
GEN_XDG_C = xdg-shell-protocol.c

all: $(BIN)

deps: $(STB_H) $(WLR_XML) $(XDG_XML)

$(STB_H):
	curl -sL $(STB_URL) -o $@

$(WLR_XML):
	mkdir -p protocols
	curl -sL $(WLR_URL) -o $@

$(XDG_XML):
	mkdir -p protocols
	curl -sL $(XDG_URL) -o $@

$(GEN_WLR_H): $(WLR_XML)
	wayland-scanner client-header $< $@

$(GEN_WLR_C): $(WLR_XML)
	wayland-scanner private-code $< $@

$(GEN_XDG_C): $(XDG_XML)
	wayland-scanner private-code $< $@

$(BIN): main.c $(STB_H) $(GEN_WLR_H) $(GEN_WLR_C) $(GEN_XDG_C)
	$(CC) $(CFLAGS) $(WAY_CFLAGS) -o $@ main.c $(GEN_WLR_C) $(GEN_XDG_C) $(WAY_LIBS)

clean:
	rm -f $(BIN) $(GEN_WLR_H) $(GEN_WLR_C) $(GEN_XDG_C)

distclean: clean
	rm -f $(STB_H)
	rm -rf protocols

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

.PHONY: all deps clean distclean install uninstall
