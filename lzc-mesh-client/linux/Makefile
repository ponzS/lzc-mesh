# LZC Mesh Linux client: GTK4 window, libsoup3 hub connection, Wayland input.
CC ?= gcc
PKGS = gtk4 libsoup-3.0 json-glib-1.0 wayland-client xkbcommon
CFLAGS += -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell pkg-config --cflags $(PKGS)) -Ibuild
LDLIBS += $(shell pkg-config --libs $(PKGS))
VERSION ?= 0.1.0
CFLAGS += -DLZC_MESH_VERSION=\"$(VERSION)\"

PROTOCOLS = wlr-layer-shell-unstable-v1 wlr-virtual-pointer-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell \
	pointer-constraints-unstable-v1 relative-pointer-unstable-v1 keyboard-shortcuts-inhibit-unstable-v1 xdg-output-unstable-v1
PROTO_H = $(PROTOCOLS:%=build/%-client-protocol.h)
PROTO_C = $(PROTOCOLS:%=build/%-protocol.c)
SRC = src/main.c src/ui.c src/layout_view.c src/net.c src/lzc.c src/wl_input.c src/config.c
OBJ = $(SRC:src/%.c=build/%.o) $(PROTO_C:.c=.o)

all: build/lzc-mesh

build/%-client-protocol.h: protocols/%.xml | build
	wayland-scanner client-header $< $@
build/%-protocol.c: protocols/%.xml | build
	wayland-scanner private-code $< $@
build/%.o: src/%.c $(PROTO_H) src/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@
build/%-protocol.o: build/%-protocol.c
	$(CC) $(CFLAGS) -c $< -o $@
build/lzc-mesh: $(OBJ)
	$(CC) $(OBJ) $(LDLIBS) -o $@
build:
	mkdir -p build

dist: build/lzc-mesh
	rm -rf build/dist && mkdir -p build/dist/lzc-mesh
	cp build/lzc-mesh lzc-mesh.desktop install.sh build/dist/lzc-mesh/
	tar -C build/dist -czf ../dist/lzc-mesh-linux-x86_64.tar.gz lzc-mesh

clean:
	rm -rf build
.PHONY: all clean dist
