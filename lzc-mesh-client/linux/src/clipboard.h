#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
// Shared clipboard (text) via wlr-data-control. Active only while the hub
// reports sharing enabled; see app.clipboard_enabled.
bool clipboard_bind(struct wl_display *display, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version);
void clipboard_start(struct wl_seat *seat);
void clipboard_set(const char *text, const char *from);
bool clipboard_available(void);
