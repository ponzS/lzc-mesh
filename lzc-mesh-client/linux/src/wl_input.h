#pragma once
#include <stdbool.h>
#include <stdint.h>
// Wayland input: edge detection, capture of local input while controlling
// another device, and injection while being controlled.
void wl_input_init(void);
void wl_input_shutdown(void);
void wl_input_set_edges(bool left, bool right, bool up, bool down);
void wl_input_capture(bool on);
void wl_input_warp(double nx, double ny);       // normalised over the local desktop
void wl_input_enter(double nx, double ny);
void wl_input_leave(void);
void wl_input_move(double nx, double ny);
void wl_input_button(int button, bool down);
void wl_input_wheel(double dx, double dy);
void wl_input_key(uint32_t code, bool down);
