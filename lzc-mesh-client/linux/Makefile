# 懒猫设备接力 (LZC Mesh) Linux client: GTK4 window, libsoup3 hub connection, Wayland input and clipboard.
CC ?= gcc
PKGS = gtk4 libsoup-3.0 json-glib-1.0 wayland-client xkbcommon cairo
CFLAGS += -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $(shell pkg-config --cflags $(PKGS)) -Ibuild
LDLIBS += $(shell pkg-config --libs $(PKGS))
VERSION ?= 0.1.2
CFLAGS += -DLZC_MESH_VERSION=\"$(VERSION)\"

PROTOCOLS = wlr-layer-shell-unstable-v1 wlr-virtual-pointer-unstable-v1 virtual-keyboard-unstable-v1 xdg-shell \
	pointer-constraints-unstable-v1 relative-pointer-unstable-v1 keyboard-shortcuts-inhibit-unstable-v1 xdg-output-unstable-v1 \
	wlr-data-control-unstable-v1
PROTO_H = $(PROTOCOLS:%=build/%-client-protocol.h)
PROTO_C = $(PROTOCOLS:%=build/%-protocol.c)
SRC = src/main.c src/ui.c src/layout_view.c src/net.c src/lzc.c src/wl_input.c src/config.c src/clipboard.c src/files.c src/tray.c src/autostart.c
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

# Portable archive: binary + desktop entry + icon + install.sh
dist: build/lzc-mesh
	rm -rf build/dist && mkdir -p build/dist/lzc-mesh ../dist
	cp build/lzc-mesh lzc-mesh.desktop lzc-mesh.png install.sh ../../LICENSE build/dist/lzc-mesh/
	tar -C build/dist -czf ../dist/lzc-mesh-linux-x86_64.tar.gz lzc-mesh

# Installer: Arch Linux package (pacman -U), built with makepkg from this tree
pkg:
	rm -rf build/pkg && mkdir -p build/pkg/src/linux ../dist
	cp -r Makefile src protocols lzc-mesh.desktop lzc-mesh.png build/pkg/src/linux/
	cp pkg/PKGBUILD build/pkg/
	cp ../../LICENSE build/pkg/LICENSE
	cd build/pkg && sed -i 's#install -Dm0644 ../../LICENSE#install -Dm0644 "$$startdir/LICENSE"#' PKGBUILD && \
		SRCDEST=$$PWD PKGDEST=$$PWD/out makepkg -f --noextract --nodeps --noconfirm >makepkg.log 2>&1 || (tail -20 makepkg.log; exit 1)
	cp build/pkg/out/lzc-mesh-$(VERSION)-1-x86_64.pkg.tar.zst ../dist/lzc-mesh-linux-x86_64.pkg.tar.zst

clean:
	rm -rf build
.PHONY: all clean dist pkg
