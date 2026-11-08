PROG = screen-translate-overlay
PREFIX = /usr/local
CC = gcc
WAYLAND_SCANNER = wayland-scanner

PKGS = wayland-client cairo xkbcommon
CPPFLAGS = $(shell pkg-config --cflags $(PKGS))
CFLAGS = -std=c99 -pedantic -Wall -Wextra -O2
LDFLAGS = $(shell pkg-config --libs $(PKGS)) -lm

PROTO_XML = protocols/wlr-layer-shell-unstable-v1.xml
PROTO_H = wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_C = wlr-layer-shell-unstable-v1-protocol.c

SRC = overlay.c $(PROTO_C)
OBJ = overlay.o $(PROTO_C:.c=.o)

all: $(PROG)

$(PROTO_H): $(PROTO_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_C): $(PROTO_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(PROTO_C:.c=.o): $(PROTO_C) $(PROTO_H)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

overlay.o: overlay.c $(PROTO_H)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(PROG): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

install: all
	install -D -m 755 $(PROG) $(DESTDIR)$(PREFIX)/bin/$(PROG)

clean:
	rm -f $(PROG) $(OBJ) $(PROTO_H) $(PROTO_C)

.PHONY: all install clean
