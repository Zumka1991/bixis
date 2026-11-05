VERSION = 0.1.0
PREFIX ?= /usr/local

PKGS = wayland-client xkbcommon freetype2 fontconfig
PKG_CONFIG ?= pkg-config
WLP = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -D_GNU_SOURCE -DVERSION=\"$(VERSION)\"
CFLAGS += -I. -Ibuild $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lutil -lm

PROTOS = xdg-shell xdg-decoration-unstable-v1 cursor-shape-v1 tablet-v2
xml_xdg-shell                  = $(WLP)/stable/xdg-shell/xdg-shell.xml
xml_xdg-decoration-unstable-v1 = $(WLP)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
xml_cursor-shape-v1            = $(WLP)/staging/cursor-shape/cursor-shape-v1.xml
xml_tablet-v2                  = $(WLP)/stable/tablet/tablet-v2.xml

PROTO_H = $(PROTOS:%=build/%-client-protocol.h)
PROTO_O = $(PROTOS:%=build/%-protocol.o)
OBJ = $(patsubst src/%.c,build/%.o,$(wildcard src/*.c))

all: build/bixis-term

build/bixis-term: $(OBJ) $(PROTO_O)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c config.h $(wildcard src/*.h) $(PROTO_H) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%-protocol.o: build/%-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

.SECONDEXPANSION:
build/%-client-protocol.h: $$(xml_%) | build
	$(SCANNER) client-header $< $@

build/%-protocol.c: $$(xml_%) | build
	$(SCANNER) private-code $< $@

build:
	mkdir -p build

check: | build
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -Isrc -o build/test-vt \
		tests/vt.c src/term.c -lutil
	./build/test-vt

install: build/bixis-term
	install -Dm755 build/bixis-term $(DESTDIR)$(PREFIX)/bin/bixis-term
	install -Dm644 bixis-term.desktop $(DESTDIR)$(PREFIX)/share/applications/bixis-term.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/bixis-term
	rm -f $(DESTDIR)$(PREFIX)/share/applications/bixis-term.desktop

clean:
	rm -rf build

.PHONY: all check install uninstall clean
