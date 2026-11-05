VERSION = 0.1.0
PREFIX ?= /usr/local

PKGS = wlroots-0.19 wayland-server xkbcommon
PKG_CONFIG ?= pkg-config
WLP = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
SCANNER = $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)

CFLAGS ?= -O2 -pipe
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DWLR_USE_UNSTABLE
CFLAGS += -DVERSION=\"$(VERSION)\" -Ibuild $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

GEN = build/xdg-shell-protocol.h build/cursor-shape-v1-protocol.h build/default_config.h
OBJ = $(patsubst src/%.c,build/%.o,$(wildcard src/*.c))

all: build/bixis

build/bixis: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(wildcard src/*.h) $(GEN) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/xdg-shell-protocol.h: $(WLP)/stable/xdg-shell/xdg-shell.xml | build
	$(SCANNER) server-header $< $@

build/cursor-shape-v1-protocol.h: $(WLP)/staging/cursor-shape/cursor-shape-v1.xml | build
	$(SCANNER) server-header $< $@

# the example config is also the built-in default
build/default_config.h: bixis.conf | build
	{ echo 'static const char default_config[] ='; \
	  sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/"&\\n"/' $<; echo ';'; } > $@

build:
	mkdir -p build

check: $(GEN) | build
	$(CC) $(CFLAGS) -g -o build/test-volume tests/volume.c src/config.c $(LDLIBS)
	./build/test-volume

install: build/bixis
	install -Dm755 build/bixis $(DESTDIR)$(PREFIX)/bin/bixis
	install -Dm644 bixis.conf $(DESTDIR)$(PREFIX)/share/bixis/bixis.conf
	install -Dm644 bixis.desktop $(DESTDIR)$(PREFIX)/share/wayland-sessions/bixis.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/bixis
	rm -rf $(DESTDIR)$(PREFIX)/share/bixis
	rm -f $(DESTDIR)$(PREFIX)/share/wayland-sessions/bixis.desktop

clean:
	rm -rf build

.PHONY: all check install uninstall clean
