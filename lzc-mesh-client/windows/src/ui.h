#pragma once
// Owner-drawn black / white / grey widgets for the Win32 client, matching the
// management page. Static text and panels are painted by the window itself;
// only interactive parts are child controls (flat buttons and toggle switches).
#include "app.h"

namespace ui {
const COLORREF BG = RGB(245, 245, 245), PANEL = RGB(255, 255, 255), LINE = RGB(226, 226, 226), TEXT = RGB(17, 17, 17),
               MUTED = RGB(119, 119, 119), SCREEN = RGB(250, 250, 250), SELF = RGB(237, 237, 237), HOVER = RGB(242, 242, 242);

struct Fonts {
    HFONT body = nullptr, bodyBold = nullptr, small = nullptr, title = nullptr, header = nullptr, mono = nullptr;
};
extern Fonts F;
void makeFonts(UINT dpi);
int S(int v);

void fillRound(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border);
void fillRect(HDC dc, RECT r, COLORREF c);
// Returns the height used.
int drawText(HDC dc, const std::wstring &s, RECT r, HFONT f, COLORREF c, UINT flags);
void drawDot(HDC dc, int cx, int cy, Level lv);

HWND button(HWND parent, const wchar_t *text, int id, bool primary = false);
HWND toggle(HWND parent, int id);
void setToggle(HWND h, bool on);
bool toggleOn(HWND h);
// WM_DRAWITEM handler for the controls above; true when handled.
bool drawItem(const DRAWITEMSTRUCT *d);
// Width a button needs for its text.
int buttonWidth(HWND h);
}  // namespace ui
