#pragma once
#include "app.h"
// Main window chrome: painted header, panels and status, plus the child controls.
namespace mainwin {
enum {
    ID_RETRY = 100, ID_OPEN, ID_COPY, ID_BOX_APPLY, ID_SWAP, ID_ELEVATE, ID_SHARING, ID_AUTOSTART, ID_BOX_EDIT, ID_LOG, ID_TRANSPORT, ID_PAUSE_ALL, ID_CLIENT,
};
void create(HWND h);
void onDpi(HWND h);
void layout(HWND h);
void paint(HWND h);
void refresh();              // status texts, pill, switches
void showElevate(bool show);
HWND logWindow();
HWND boxEdit();
void setOpenEnabled(bool on);
std::string selectedTransport();
void scroll(int code);
bool wheel(POINT point, int delta);
}
