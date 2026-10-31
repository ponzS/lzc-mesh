#pragma once
// Notification-area icon with connection status, the sharing / clipboard /
// autostart switches, open window, open management page and quit.
namespace tray {
enum { ID_TRAY_SHOW = 900, ID_TRAY_SHARING, ID_TRAY_CLIPBOARD, ID_TRAY_AUTOSTART, ID_TRAY_OPEN, ID_TRAY_QUIT };
void add(HWND owner);
void remove();
void refresh();                     // icon + tooltip from current state
void showMenu(HWND owner);          // on right click
void onTaskbarCreated(HWND owner);  // Explorer restarted: re-add
void onThemeChanged();              // light / dark taskbar switched
}
