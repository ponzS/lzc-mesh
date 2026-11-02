// Drag-to-arrange screen layout (child window). Same model as the web page and the
// macOS client: devices drawn scaled to fit; releasing a block sends its new
// position to the hub, which snaps it and broadcasts the result.
#include "layoutview.h"

#include <windowsx.h>

#include <algorithm>

#include "net.h"
#include "ui.h"

namespace layoutview {
namespace {
HWND view;
double scale = 1, ox = 0, oy = 0;
int drag = -1;
bool moved = false;
POINT dragOffset{}, dragPos{};

using ui::S;

void compute(RECT rc) {
    if (g.devices.empty()) return;
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (auto &d : g.devices) { x0 = std::min(x0, d.x); y0 = std::min(y0, d.y); x1 = std::max(x1, d.x + d.w); y1 = std::max(y1, d.y + d.h); }
    double pad = S(24), W = rc.right - 2 * pad, H = rc.bottom - 2 * pad;
    // Leave a little room around the group so a block can be dropped next to any side.
    scale = std::max(0.001, std::min({W / ((x1 - x0) * 1.25), H / ((y1 - y0) * 1.25), 0.25 * ui::S(96) / 96.0}));
    ox = pad + (W - (x1 - x0) * scale) / 2 - x0 * scale;
    oy = pad + (H - (y1 - y0) * scale) / 2 - y0 * scale;
}

RECT restingRect(int i) {
    auto &d = g.devices[i];
    int x = (int)(ox + d.x * scale), y = (int)(oy + d.y * scale);
    return RECT{x, y, x + (int)(d.w * scale), y + (int)(d.h * scale)};
}

RECT rectOf(int i) {
    RECT r = restingRect(i);
    if (i == drag) OffsetRect(&r, dragPos.x - r.left, dragPos.y - r.top);
    return r;
}

void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bmp);
    ui::fillRect(dc, rc, ui::PANEL);
    ui::fillRound(dc, rc, S(12), RGB(251, 251, 251), RGB(236, 236, 236));
    for (int x = S(10); x < rc.right - S(4); x += S(16))
        for (int y = S(10); y < rc.bottom - S(4); y += S(16)) SetPixel(dc, x, y, RGB(222, 222, 222));
    if (g.devices.empty()) {
        ui::drawText(dc, g.hubConnected ? L"暂无设备" : L"连接后显示屏幕布局", rc, ui::F.body, ui::MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        if (drag < 0) compute(rc);
        std::vector<int> order;
        for (int i = 0; i < (int)g.devices.size(); i++) if (i != drag) order.push_back(i);
        if (drag >= 0 && drag < (int)g.devices.size()) order.push_back(drag);  // dragged block on top
        for (int i : order) {
            auto &d = g.devices[i];
            RECT r = rectOf(i);
            bool me = d.id == g.deviceId, active = d.id == g.activeSource || d.id == g.activeTarget;
            if (i == drag) {
                RECT sh = r;
                OffsetRect(&sh, 0, S(3));
                ui::fillRound(dc, sh, S(12), RGB(225, 225, 225), RGB(225, 225, 225));
            }
            COLORREF border = active ? ui::TEXT : d.online ? RGB(150, 150, 150) : RGB(200, 200, 200);
            ui::fillRound(dc, r, S(12), me ? ui::SELF : ui::SCREEN, border);
            if (active) {
                RECT in = r;
                InflateRect(&in, -1, -1);
                HGDIOBJ op = SelectObject(dc, CreatePen(PS_SOLID, 1, ui::TEXT));
                HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, in.left, in.top, in.right, in.bottom, S(10), S(10));
                DeleteObject(SelectObject(dc, op));
                SelectObject(dc, obr);
            }
            bool compact = r.right - r.left < S(120) || r.bottom - r.top < S(64);
            const wchar_t *plat = d.platform == "macos" ? L"macOS" : d.platform == "windows" ? L"Windows" : d.platform == "linux" ? L"Linux" : L"?";
            std::wstring title = widen(d.name);
            std::wstring meta = compact ? std::wstring(plat) : fmt(L"%ls · %.0f×%.0f", plat, d.w, d.h);
            RECT t1 = r, t2 = r;
            int mid = (r.top + r.bottom) / 2;
            t1.bottom = mid + S(1);
            t2.top = mid + S(2);
            InflateRect(&t1, -S(6), 0);
            InflateRect(&t2, -S(6), 0);
            ui::drawText(dc, title, t1, compact ? ui::F.small : ui::F.bodyBold, d.online ? ui::TEXT : ui::MUTED, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            ui::drawText(dc, meta, t2, ui::F.small, ui::MUTED, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            const wchar_t *role = !d.enabled ? L"已关闭" : d.id == g.activeTarget ? L"受控中" : d.id == g.activeSource ? L"控制中" : d.online && !d.sharing ? L"已暂停" : !d.online ? L"离线" : me ? L"本机" : L"";
            if (*role) {
                RECT rr{r.left, r.top + S(4), r.right - S(8), r.top + S(22)};
                ui::drawText(dc, role, rr, ui::F.small, active ? ui::TEXT : ui::MUTED, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if (drag < 0) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        bool over = false;
        for (int i = 0; i < (int)g.devices.size() && g.hubConnected; i++) { RECT r = rectOf(i); over |= PtInRect(&r, p) != 0; }
        SetCursor(LoadCursorW(nullptr, drag >= 0 ? IDC_SIZEALL : over ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!g.hubConnected) return 0;
        for (int i = (int)g.devices.size() - 1; i >= 0; i--) {
            RECT r = restingRect(i);
            if (!PtInRect(&r, p)) continue;
            dragPos = POINT{r.left, r.top};
            dragOffset = POINT{p.x - r.left, p.y - r.top};
            drag = i;
            moved = false;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            break;
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag >= 0) {
            dragPos = POINT{GET_X_LPARAM(l) - dragOffset.x, GET_Y_LPARAM(l) - dragOffset.y};
            moved = true;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (drag >= 0) {
            int i = drag;
            drag = -1;
            if (m == WM_LBUTTONUP) ReleaseCapture();
            if (moved && i < (int)g.devices.size()) {
                auto &d = g.devices[i];
                hubws::send(JsonOut().s("t", "layout").s("id", d.id).n("x", (dragPos.x - ox) / scale).n("y", (dragPos.y - oy) / scale).str());
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

HWND create(HWND parent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LzcMeshLayout";
    wc.hCursor = nullptr;
    RegisterClassW(&wc);
    view = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, nullptr, nullptr, nullptr);
    return view;
}

void refresh() {
    if (view && drag < 0) InvalidateRect(view, nullptr, FALSE);
}
}  // namespace layoutview
