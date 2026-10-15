CC = gcc
CFLAGS = -Wall -Wextra -O2 -I.
LDFLAGS = -lwayland-client -lm

WAYLAND_SCANNER = wayland-scanner

PROTOCOL_HEADERS = \
	xdg-shell-client-protocol.h \
	wlr-layer-shell-unstable-v1-client-protocol.h

PROTOCOL_SOURCES = \
	xdg-shell-protocol.c \
	wlr-layer-shell-unstable-v1-protocol.c

TARGET = wl-paper

all: $(TARGET)

xdg-shell-client-protocol.h: protocols/xdg-shell.xml
	$(WAYLAND_SCANNER) client-header $< $@

xdg-shell-protocol.c: protocols/xdg-shell.xml
	$(WAYLAND_SCANNER) private-code $< $@

wlr-layer-shell-unstable-v1-client-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: protocols/wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

$(TARGET): $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES) main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(PROTOCOL_SOURCES) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)

.PHONY: all clean
