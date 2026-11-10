PROG = screen-translate-overlay
WATCHER = screen-translate-watcher
PREFIX = /usr/local
CC = gcc
WAYLAND_SCANNER = wayland-scanner

PKGS = wayland-client cairo xkbcommon
CPPFLAGS = $(shell pkg-config --cflags $(PKGS))
CFLAGS = -std=c99 -pedantic -Wall -Wextra -O2
LDFLAGS = $(shell pkg-config --libs $(PKGS)) -lm

LAYER_XML = protocols/wlr-layer-shell-unstable-v1.xml
LAYER_H = wlr-layer-shell-unstable-v1-client-protocol.h
LAYER_C = wlr-layer-shell-unstable-v1-protocol.c

VKBD_XML = protocols/virtual-keyboard-unstable-v1.xml
VKBD_H = virtual-keyboard-unstable-v1-client-protocol.h
VKBD_C = virtual-keyboard-unstable-v1-protocol.c

SRC = overlay.c $(LAYER_C) $(VKBD_C)
OBJ = overlay.o $(LAYER_C:.c=.o) $(VKBD_C:.c=.o)

all: $(PROG) $(WATCHER)

$(LAYER_H): $(LAYER_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(LAYER_C): $(LAYER_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(VKBD_H): $(VKBD_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(VKBD_C): $(VKBD_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(LAYER_C:.c=.o): $(LAYER_C) $(LAYER_H)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(VKBD_C:.c=.o): $(VKBD_C) $(VKBD_H)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

overlay.o: overlay.c $(LAYER_H) $(VKBD_H)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(PROG): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

$(WATCHER): watcher.c
	$(CC) $(CFLAGS) $< -o $@

install: all
	install -D -m 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG)
	install -D -m 755 $(WATCHER) $(DESTDIR)$(PREFIX)/bin/$(WATCHER)
	install -D -m 755 render.py $(DESTDIR)$(PREFIX)/bin/screen-translate-render
	install -D -m 755 screen-translate.sh $(DESTDIR)$(PREFIX)/bin/screen-translate

clean:
	rm -f $(PROG) $(WATCHER) $(OBJ) $(LAYER_H) $(LAYER_C) $(VKBD_H) $(VKBD_C)

.PHONY: all install clean
