PROG = screen-translate-overlay
WATCHER = screen-translate-watcher
VERSION = 1.0.2
PREFIX = /usr/local
CC = cc
WAYLAND_SCANNER = wayland-scanner

PKGS = wayland-client cairo xkbcommon
CPPFLAGS = -DVERSION=\"$(VERSION)\" $(shell pkg-config --cflags $(PKGS))
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2
LDFLAGS = $(shell pkg-config --libs $(PKGS))

LAYER_XML = protocols/wlr-layer-shell-unstable-v1.xml
LAYER_H = wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_C = wlr-layer-shell-unstable-v1-protocol.c

VKBD_XML = protocols/virtual-keyboard-unstable-v1.xml
VKBD_H = virtual-keyboard-unstable-v1-client-protocol.h
VKBD_C = virtual-keyboard-unstable-v1-protocol.c

SRC = overlay.c $(LAYER_C) $(VKBD_C)
OBJ = $(SRC:.c=.o)

all: $(PROG) $(WATCHER)

$(OBJ): $(LAYER_H) $(VKBD_H)

$(LAYER_H): $(LAYER_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(LAYER_C): $(LAYER_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(VKBD_H): $(VKBD_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(VKBD_C): $(VKBD_XML)
	$(WAYLAND_SCANNER) private-code $< $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(PROG): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(WATCHER): watcher.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -D -m 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG)
	install -D -m 755 $(WATCHER) $(DESTDIR)$(PREFIX)/bin/$(WATCHER)
	install -D -m 755 render.py $(DESTDIR)$(PREFIX)/bin/screen-translate-render
	install -D -m 755 screen-translate.sh $(DESTDIR)$(PREFIX)/bin/screen-translate

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(PROG)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(WATCHER)
	rm -f $(DESTDIR)$(PREFIX)/bin/screen-translate-render
	rm -f $(DESTDIR)$(PREFIX)/bin/screen-translate

clean:
	rm -f $(PROG) $(WATCHER) $(OBJ) $(LAYER_H) $(LAYER_C) $(VKBD_H) $(VKBD_C)

.PHONY: all install uninstall clean
