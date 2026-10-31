#pragma once
#include <stdbool.h>
// Start at login via an XDG autostart entry (~/.config/autostart/lzc-mesh.desktop).
bool autostart_enabled(void);
bool autostart_set(bool on, char **error);
