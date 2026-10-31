#include "ui.h"
#include <algorithm>

namespace ui {
Fonts F;
namespace {
UINT curDpi = 96;
WNDPROC buttonDefProc;

// Hover tracking + hand cursor for our owner-drawn buttons.
LRESULT CALLBACK buttonSub(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, L"hover")) {
            SetPropW(h, L"hover", (HANDLE)1);
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, L"hover");
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    }
    return CallWindowProcW(buttonDefProc, h, m, w, l);
}

HWND makeOwnerButton(HWND parent, const wchar_t *text, int id, LONG_PTR kind) {
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SetPropW(h, L"kind", (HANDLE)kind);
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)buttonSub);
    if (!buttonDefProc) buttonDefProc = prev;
    return h;
}
}  // namespace

int S(int v) { return MulDiv(v, (int)curDpi, 96); }

void makeFonts(UINT dpi) {
    curDpi = dpi;
    for (HFONT f : {F.body, F.bodyBold, F.small, F.title, F.header, F.mono}) if (f) DeleteObject(f);
    auto mk = [](int size, int weight, const wchar_t *face) {
        return CreateFontW(-S(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    F.body = mk(13, FW_NORMAL, L"Microsoft YaHei UI");
    F.bodyBold = mk(13, FW_SEMIBOLD, L"Microsoft YaHei UI");
    F.small = mk(12, FW_NORMAL, L"Microsoft YaHei UI");
    F.title = mk(14, FW_SEMIBOLD, L"Microsoft YaHei UI");
    F.header = mk(16, FW_SEMIBOLD, L"Microsoft YaHei UI");
    F.mono = mk(12, FW_NORMAL, L"Consolas");
}

void fillRect(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void fillRound(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

int drawText(HDC dc, const std::wstring &s, RECT r, HFONT f, COLORREF c, UINT flags) {
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT m = r;
    int h = DrawTextW(dc, s.c_str(), (int)s.size(), &m, flags | DT_CALCRECT);
    if (!(flags & DT_CALCRECT)) DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags);
    SelectObject(dc, of);
    return h;
}

void drawDot(HDC dc, int cx, int cy, Level lv) {
    int r = S(4);
    COLORREF fill = lv == L_OK ? TEXT : lv == L_WARN ? RGB(136, 136, 136) : lv == L_ERROR ? PANEL : RGB(204, 204, 204);
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, lv == L_ERROR ? std::max(1, S(3) / 2) : 1, lv == L_ERROR ? TEXT : fill);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

HWND button(HWND parent, const wchar_t *text, int id, bool primary) { return makeOwnerButton(parent, text, id, primary ? 2 : 1); }
HWND toggle(HWND parent, int id) { return makeOwnerButton(parent, L"", id, 3); }
void setToggle(HWND h, bool on) {
    if (toggleOn(h) == on) return;
    SetPropW(h, L"on", (HANDLE)(INT_PTR)(on ? 1 : 0));
    InvalidateRect(h, nullptr, FALSE);
}
bool toggleOn(HWND h) { return GetPropW(h, L"on") != nullptr; }

int buttonWidth(HWND h) {
    wchar_t t[128];
    GetWindowTextW(h, t, 128);
    HDC dc = GetDC(h);
    HGDIOBJ of = SelectObject(dc, F.body);
    SIZE sz{};
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    SelectObject(dc, of);
    ReleaseDC(h, dc);
    return sz.cx + S(24);
}

bool drawItem(const DRAWITEMSTRUCT *d) {
    if (d->CtlType != ODT_BUTTON) return false;
    INT_PTR kind = (INT_PTR)GetPropW(d->hwndItem, L"kind");
    if (!kind) return false;
    RECT rc = d->rcItem;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    // Double buffer to avoid flicker on hover.
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bmp);
    COLORREF parentBg = (COLORREF)(INT_PTR)GetPropW(d->hwndItem, L"bg");
    fillRect(dc, RECT{0, 0, w, h}, GetPropW(d->hwndItem, L"bgset") ? parentBg : PANEL);
    bool hover = GetPropW(d->hwndItem, L"hover") != nullptr, pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED;
    if (kind == 3) {
        bool on = toggleOn(d->hwndItem);
        int th = S(20), tw = S(34), x = w - tw, y = (h - th) / 2;
        COLORREF track = disabled ? RGB(224, 224, 224) : on ? TEXT : RGB(204, 204, 204);
        fillRound(dc, RECT{x, y, x + tw, y + th}, th, track, track);
        int k = th - S(4), kx = on ? x + tw - k - S(2) : x + S(2);
        fillRound(dc, RECT{kx, y + S(2), kx + k, y + S(2) + k}, k, PANEL, PANEL);
    } else {
        bool primary = kind == 2;
        COLORREF fill = primary ? (pressed ? RGB(60, 60, 60) : hover ? RGB(51, 51, 51) : TEXT) : (pressed ? RGB(232, 232, 232) : hover ? HOVER : PANEL);
        COLORREF border = primary ? TEXT : hover ? RGB(158, 158, 158) : LINE;
        fillRound(dc, RECT{0, 0, w, h}, S(10), fill, border);
        wchar_t t[128];
        GetWindowTextW(d->hwndItem, t, 128);
        COLORREF fg = disabled ? RGB(170, 170, 170) : primary ? PANEL : TEXT;
        drawText(dc, t, RECT{0, 0, w, h}, F.body, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    BitBlt(d->hDC, rc.left, rc.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    return true;
}
}  // namespace ui
