#pragma once
#include <stdbool.h>
// Tray icon (StatusNotifierItem + dbusmenu) with connection status, the sharing /
// clipboard / autostart switches, open window, open management page and quit.
void tray_start(void);
void tray_refresh(void);       // after any state change; no-op if nothing visible changed
void tray_menu_changed(void);  // force the host to refetch the menu
bool tray_available(void);
