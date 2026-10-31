#pragma once
#include <stdbool.h>
#include <stdint.h>
void net_connect(void);
void net_disconnect(void);
bool net_busy(void);
void net_send_hello(void);
void net_send_edge(const char *dir, double pos);
void net_send_motion(double dx, double dy);
void net_send_button(int button, bool down);
void net_send_wheel(double dx, double dy);
void net_send_key(uint32_t code, bool down);
void net_send_release(void);
void net_send_layout(const char *id, double x, double y);
void net_send_forget(const char *id);
void net_send_settings(bool swap);
void net_send_clipboard(const char *text);
void net_send_sharing(bool on);
void net_send_clipboard_setting(bool on);
