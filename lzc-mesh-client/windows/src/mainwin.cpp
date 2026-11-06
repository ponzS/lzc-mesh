// Main window layout and painting. Static text and panels are drawn directly on the
// window (no STATIC controls), so everything uses the same palette and fonts.
//
//   header:  title · status pill · control line ................ 键鼠共享 [switch]
//   left:    屏幕布局 panel (layout view)  /  文件共享 panel (list + buttons)
//   right:   连接状态 panel  /  设置 panel  /  事件日志 panel
#include "mainwin.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>

#include "autostart.h"
#include "files.h"
#include "layoutview.h"
#include "ui.h"

namespace mainwin {
namespace {
using ui::S;

constexpr const wchar_t *labels[] = {L"懒猫微服客户端", L"懒猫网络", L"登录会话", L"设备接力应用", L"中枢连接", L"输入权限", L"控制状态", L"本机屏幕", L"共享剪贴板", L"输入通道", L"光标通道", L"键盘通道", L"设备信任"};
static_assert(sizeof(labels)/sizeof(*labels)==S_COUNT,"Every status needs a non-null painting label");

HWND hLayout, hLog, hBox, hSharing, hSwap, hAutoCenter, hAutostart, hRetry, hOpen, hCopy, hBoxApply, hElevate, hUpload, hDownload, hDelete, hTransport, hPause, hClient;
bool elevateShown = false;

// Geometry computed in layout(), used by paint().
RECT pLayout, pFiles, pStatus, pSettings, pLog, layoutHint, filesStatus, filesEmpty;
RECT statusRows[S_COUNT];
RECT settingText[4], boxLabel, boxFrame, modeLabel, modeNote, rightViewport;
int keyWidth = 0, rightOffset = 0, rightHeight = 0, rightPage = 0;
int wheelRemainder = 0;
const char *modes[] = {"auto", "bluetooth", "direct", "relay"};

const wchar_t *settingTitle[4] = {L"客户端运行", L"开机自动启动", L"交换 Command 和 Ctrl", L"光标共享，自动居中"};
const wchar_t *settingDetail[4] = {L"关闭全部共享，保留管理连接以便远程开启", L"登录系统后自动在托盘运行", L"Mac 与 Windows/Linux 互控时对调这两个键", L"跨屏切换时自动将鼠标光标移到屏幕中央"};
const wchar_t *layoutHintText = L"拖动屏幕调整相对位置，松手后自动贴边并同步到所有设备。控制其他设备时按 Ctrl+Alt+Esc 收回光标。";

std::wstring summary(Level &lv) {
    if (g.hubConnected && !g.authVerified) { lv=L_WARN; return L"设备未获信任 · 共享停用"; }
    if (g.hubConnected && g.deviceReady && !g.deviceEnabled) { lv = L_WARN; return L"客户端已关闭 · 可远程开启"; }
    if (g.hubConnected) {
        int online = 0;
        for (auto &d : g.devices) online += d.online;
        lv = L_OK;
        return fmt(L"已连接 · %d 台设备在线", online);
    }
    for (int k : {S_LZC_CLIENT, S_LZC_NETWORK, S_LOGIN, S_SERVICE, S_HUB})
        if (g.status[k].level == L_ERROR || g.status[k].level == L_WARN) {
            lv = g.status[k].level;
            return std::wstring(labels[k]) + L"：" + g.status[k].text;
        }
    lv = L_UNKNOWN;
    return L"正在连接…";
}

std::wstring controlLine() {
    auto name = [](const std::string &id) {
        for (auto &d : g.devices) if (d.id == id) return widen(d.name);
        return widen(id);
    };
    if (g.captured) return L"正在控制 " + name(g.activeTarget);
    if (g.controlled) return name(g.activeSource) + L" 正在控制本机";
    return L"";
}

void panel(HDC dc, RECT r, const wchar_t *title) {
    ui::fillRound(dc, r, S(14), ui::PANEL, ui::LINE);
    RECT t{r.left + S(16), r.top + S(13), r.right - S(16), r.top + S(36)};
    ui::drawText(dc, title, t, ui::F.title, ui::TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
}

int measure(HWND h, const std::wstring &s, int width, HFONT f) {
    HDC dc = GetDC(h);
    RECT r{0, 0, width, 0};
    int hgt = ui::drawText(dc, s, r, f, 0, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    ReleaseDC(h, dc);
    return hgt;
}

void place(HWND c, int x, int y, int w, int h) { SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE); }
void placeRight(HWND c, int x, int y, int w, int h) {
    place(c, x, y, w, h);
    // The left column and header do not scroll. Clip child windows as well as
    // painted text, so controls never cover the header when scrolled out.
    RECT visible{std::max(x, (int)rightViewport.left), std::max(y, (int)rightViewport.top),
                 std::min(x + w, (int)rightViewport.right), std::min(y + h, (int)rightViewport.bottom)};
    HRGN region = CreateRectRgn(std::max(0, (int)visible.left - x), std::max(0, (int)visible.top - y),
                                std::max(0, (int)visible.right - x), std::max(0, (int)visible.bottom - y));
    SetWindowRgn(c, region, TRUE); // Windows owns region after success.
}
void setBg(HWND c, COLORREF col) {
    SetPropW(c, L"bg", (HANDLE)(INT_PTR)col);
    SetPropW(c, L"bgset", (HANDLE)1);
}

const int HEADER = 64;  // header height (96-dpi units)
}  // namespace

HWND logWindow() { return hLog; }
HWND boxEdit() { return hBox; }

void create(HWND h) {
    ui::makeFonts(GetDpiForWindow(h));
    hLayout = layoutview::create(h);
    hSharing = ui::toggle(h, ID_SHARING);
    setBg(hSharing, ui::BG);
    hAutostart = ui::toggle(h, ID_AUTOSTART);
    hClient = ui::toggle(h, ID_CLIENT);
    hSwap = ui::toggle(h, ID_SWAP);
    hAutoCenter = ui::toggle(h, ID_AUTOCENTER);
    hTransport = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                 0, 0, 10, 200, h, (HMENU)(INT_PTR)ID_TRANSPORT, nullptr, nullptr);
    for (const auto *title : {L"自动（局域网优先）", L"仅蓝牙（实验）", L"仅局域网直连", L"仅中枢转发"}) SendMessageW(hTransport, CB_ADDSTRING, 0, (LPARAM)title);
    hPause = ui::button(h, L"暂停全部键鼠接力", ID_PAUSE_ALL);
    hRetry = ui::button(h, L"重新检测", ID_RETRY);
    hOpen = ui::button(h, L"打开管理页面", ID_OPEN);
    hCopy = ui::button(h, L"复制诊断信息", ID_COPY);
    hElevate = ui::button(h, L"以管理员身份运行", ID_ELEVATE, true);
    ShowWindow(hElevate, SW_HIDE);
    hBoxApply = ui::button(h, L"应用", ID_BOX_APPLY);
    hUpload = ui::button(h, L"上传文件", ID_FILES_UPLOAD, true);
    hDownload = ui::button(h, L"下载", ID_FILES_DOWNLOAD);
    hDelete = ui::button(h, L"删除", ID_FILES_DELETE);
    // Borderless edit; paint() draws a rounded frame around it.
    hBox = CreateWindowExW(0, L"EDIT", widen(g.boxOverride).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, h,
                           (HMENU)(INT_PTR)ID_BOX_EDIT, nullptr, nullptr);
    SendMessageW(hBox, EM_SETCUEBANNER, TRUE, (LPARAM)L"默认使用懒猫客户端的微服，例如 ponzs");
    hLog = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, h,
                           (HMENU)(INT_PTR)ID_LOG, nullptr, nullptr);
    SendMessageW(hLog, EM_SETLIMITTEXT, 0, 0);
    SetWindowTheme(hLog, L"Explorer", nullptr);  // thin modern scroll bar
    files::create(h, ui::F.body);
    onDpi(h);
}

void onDpi(HWND h) {
    ui::makeFonts(GetDpiForWindow(h));
    for (HWND c : {hBox, hTransport}) SendMessageW(c, WM_SETFONT, (WPARAM)ui::F.body, TRUE);
    SendMessageW(hLog, WM_SETFONT, (WPARAM)ui::F.mono, TRUE);
    // EDIT inner margins so text doesn't touch the border.
    SendMessageW(hBox, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
    files::setFont(ui::F.body);
    layout(h);
}

void layout(HWND h) {
    if (IsIconic(h)) return;
    RECT rc;
    if (!GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    const int pad = S(20), gap = S(14), inner = S(16), titleH = S(40);
    const int rightW = std::min(S(450), std::max(S(360), (int)(rc.right * 0.38)));
    int top = S(HEADER);

    // Header switch
    place(hSharing, rc.right - pad - S(36), (top - S(22)) / 2, S(36), S(22));

    // Right column
    int rx = rc.right - pad - rightW, rw = rightW, cw = rw - 2 * inner;
    rightViewport = RECT{rx, top, rx + rw, std::max((LONG)top, rc.bottom - pad)};
    rightPage = rightViewport.bottom - rightViewport.top;
    keyWidth = S(96);
    int textW = cw - S(16) - keyWidth;
    int y = top - rightOffset + titleH;
    for (int i = 0; i < S_COUNT; i++) {
        int th = std::max(S(18), measure(h, g.status[i].text, textW, ui::F.body));
        statusRows[i] = RECT{rx + inner, y, rx + rw - inner, y + th};
        y += th + S(7);
    }
    int by = y + S(4), bh = S(30);
    int bx = rx + inner;
    if (elevateShown) {
        int ew = ui::buttonWidth(hElevate);
        placeRight(hElevate, bx, by, ew, bh);
        by += bh + S(8);
    }
    for (HWND b : {hRetry, hOpen, hCopy}) {
        int bw = ui::buttonWidth(b);
        placeRight(b, bx, by, bw, bh);
        bx += bw + S(8);
    }
    pStatus = RECT{rx, top - rightOffset, rx + rw, by + bh + inner};

    // Settings
    y = pStatus.bottom + gap;
    int sy = y + titleH;
    HWND toggles[4] = {hClient, hAutostart, hSwap, hAutoCenter};
    for (int i = 0; i < 4; i++) {
        settingText[i] = RECT{rx + inner, sy, rx + rw - inner - S(48), sy + S(38)};
        placeRight(toggles[i], rx + rw - inner - S(36), sy + S(2), S(36), S(22));
        sy += S(48);
    }
    modeLabel = RECT{rx + inner, sy, rx + rw - inner, sy + S(20)};
    sy += S(24);
    placeRight(hTransport, rx + inner, sy, cw, S(180));
    sy += S(32);
    modeNote = RECT{rx + inner, sy, rx + rw - inner, sy + S(18)};
    sy += S(26);
    placeRight(hPause, rx + inner, sy, ui::buttonWidth(hPause), S(30));
    sy += S(42);
    boxLabel = RECT{rx + inner, sy, rx + rw - inner, sy + S(20)};
    sy += S(24);
    int applyW = ui::buttonWidth(hBoxApply);
    boxFrame = RECT{rx + inner, sy, rx + rw - inner - applyW - S(8), sy + S(30)};
    placeRight(hBox, boxFrame.left + S(9), boxFrame.top + S(6), boxFrame.right - boxFrame.left - S(18), S(18));
    placeRight(hBoxApply, rx + rw - inner - applyW, sy, applyW, S(30));
    sy += S(30);
    pSettings = RECT{rx, y, rx + rw, sy + inner};

    // The right column scrolls independently; status details, mode selection
    // and a useful event log remain accessible at the minimum window size.
    y = pSettings.bottom + gap;
    pLog = RECT{rx, y, rx + rw, std::max(y + S(200), (int)rc.bottom - pad - rightOffset)};
    rightHeight = pLog.bottom + rightOffset - top;
    const int clamped = std::clamp(rightOffset, 0, std::max(0, rightHeight - rightPage));
    if (clamped != rightOffset) { rightOffset = clamped; layout(h); return; }
    SCROLLINFO info{sizeof info, SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    info.nMax = std::max(0, rightHeight - 1); info.nPage = rightPage; info.nPos = rightOffset;
    SetScrollInfo(h, SB_VERT, &info, TRUE);
    placeRight(hLog, rx + inner, y + titleH, cw, pLog.bottom - y - titleH - inner);

    // Left column: layout grows, files gets a fixed share
    int lx = pad, lw = rx - gap - pad;
    int filesH = std::max(S(220), (int)((rc.bottom - top - pad) * 0.38));
    pFiles = RECT{lx, (int)rc.bottom - pad - filesH, lx + lw, (int)rc.bottom - pad};
    pLayout = RECT{lx, top, lx + lw, pFiles.top - gap};
    int hintH = measure(h, layoutHintText, lw - 2 * inner, ui::F.small);
    layoutHint = RECT{lx + inner, top + titleH, lx + lw - inner, top + titleH + hintH};
    place(hLayout, lx + inner, layoutHint.bottom + S(10), lw - 2 * inner, pLayout.bottom - inner - layoutHint.bottom - S(10));

    // Files panel: buttons in the title row, list, status line
    int fx = pFiles.right - inner;
    for (HWND b : {hUpload, hDelete, hDownload}) {
        int bw = ui::buttonWidth(b);
        fx -= bw;
        place(b, fx, pFiles.top + S(8), bw, S(30));
        fx -= S(8);
    }
    int listTop = pFiles.top + titleH + S(4), statusH = S(20);
    RECT listR{lx + inner, listTop, lx + lw - inner, pFiles.bottom - inner - statusH - S(6)};
    files::layout(listR.left, listR.top, listR.right - listR.left, listR.bottom - listR.top);
    filesEmpty = listR;
    filesStatus = RECT{lx + inner, listR.bottom + S(6), lx + lw - inner, listR.bottom + S(6) + statusH};
    InvalidateRect(h, nullptr, FALSE);
}

void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bmp);
    ui::fillRect(dc, rc, ui::BG);

    // Header
    const int pad = S(20), top = S(HEADER);
    int x = pad;
    RECT t{x, 0, x + S(200), top};
    ui::drawText(dc, L"懒猫设备接力", t, ui::F.header, ui::TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    {
        HGDIOBJ of = SelectObject(dc, ui::F.header);
        SIZE sz;
        GetTextExtentPoint32W(dc, L"懒猫设备接力", 6, &sz);
        SelectObject(dc, of);
        x = pad + sz.cx + S(20);
    }
    Level lv;
    std::wstring line = summary(lv);
    ui::drawDot(dc, x + S(4), top / 2, lv);
    std::wstring ctrl = controlLine();
    int switchLeft = rc.right - pad - S(36) - S(12);
    RECT sharingLabel{switchLeft - S(80), 0, switchLeft, top};
    ui::drawText(dc, L"键鼠共享", sharingLabel, ui::F.bodyBold, ui::TEXT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT pill{x + S(16), 0, sharingLabel.left - S(16), top};
    std::wstring full = ctrl.empty() ? line : line + L"   ·   " + ctrl;
    ui::drawText(dc, full, pill, ui::F.body, lv == L_OK ? ui::TEXT : ui::MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Panels
    panel(dc, pLayout, L"屏幕布局");
    ui::drawText(dc, layoutHintText, layoutHint, ui::F.small, ui::MUTED, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    panel(dc, pFiles, L"文件共享");
    if (files::empty()) {
        // The list is hidden while empty, so this text is visible.
        ui::drawText(dc, g.deviceReady && !g.deviceEnabled ? L"客户端已关闭，开启后恢复文件共享" : g.operational() ? L"还没有共享文件。点击“上传文件”或把文件拖到窗口上。" : L"连接后显示共享文件", filesEmpty, ui::F.body, ui::MUTED,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    ui::drawText(dc, files::status(), filesStatus, ui::F.small, ui::MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, rightViewport.left, rightViewport.top, rightViewport.right, rightViewport.bottom);
    panel(dc, pStatus, L"连接状态");
    for (int i = 0; i < S_COUNT; i++) {
        RECT r = statusRows[i];
        ui::drawDot(dc, r.left + S(4), r.top + S(9), g.status[i].level);
        RECT k{r.left + S(16), r.top, r.left + S(16) + keyWidth, r.bottom};
        ui::drawText(dc, labels[i], k, ui::F.body, ui::MUTED, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        RECT v{k.right, r.top, r.right, r.bottom};
        UINT wrap = DT_WORDBREAK;
        ui::drawText(dc, g.status[i].text, v, g.status[i].level == L_ERROR ? ui::F.bodyBold : ui::F.body, ui::TEXT, DT_LEFT | DT_TOP | wrap | DT_NOPREFIX);
    }

    panel(dc, pSettings, L"设置");
    for (int i = 0; i < 4; i++) {
        RECT r = settingText[i];
        RECT a{r.left, r.top, r.right, r.top + S(19)};
        ui::drawText(dc, settingTitle[i], a, ui::F.body, ui::TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        RECT b{r.left, r.top + S(20), r.right, r.bottom};
        ui::drawText(dc, settingDetail[i], b, ui::F.small, ui::MUTED, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    ui::drawText(dc, L"输入通道", modeLabel, ui::F.body, ui::TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    ui::drawText(dc, L"仅蓝牙 / 局域网不可用时不回退", modeNote, ui::F.small, ui::MUTED, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    ui::drawText(dc, L"使用的微服", boxLabel, ui::F.body, ui::TEXT, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    ui::fillRound(dc, boxFrame, S(10), ui::PANEL, GetFocus() == hBox ? RGB(150, 150, 150) : ui::LINE);

    panel(dc, pLog, L"事件日志");
    RestoreDC(dc, saved);

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void refresh() {
    if (!hSharing) return;
    ui::setToggle(hSharing, g.sharingEnabled);
    ui::setToggle(hClient, g.deviceEnabled);
    EnableWindow(hClient, g.hubConnected && g.deviceReady && g.deviceControl);
    EnableWindow(hSharing, g.operational());
    for (HWND b : {hUpload, hDownload, hDelete, hLayout}) EnableWindow(b, g.operational());
    ui::setToggle(hSwap, g.swapCmdCtrl);
    ui::setToggle(hAutoCenter, g.autoCenter);
    ui::setToggle(hAutostart, autostart::enabled());
    int selected = 0;
    for (int i = 0; i < 4; i++) if (g.transportMode == modes[i]) selected = i;
    if (SendMessageW(hTransport, CB_GETCURSEL, 0, 0) != selected) SendMessageW(hTransport, CB_SETCURSEL, selected, 0);
    EnableWindow(hTransport, g.hubConnected && g.authVerified && g.nativeProtocol);
    EnableWindow(hPause, g.hubConnected && g.authVerified && g.nativeProtocol);
    SetWindowTextW(hPause, g.inputEnabled ? L"暂停全部键鼠接力" : L"恢复全部键鼠接力");
    layoutview::refresh();
    // Status text heights can change; relayout repositions the buttons under them.
    if (IsWindowVisible(g.hwnd)) layout(g.hwnd);
}

void showElevate(bool show) {
    if (show == elevateShown) return;
    elevateShown = show;
    ShowWindow(hElevate, show ? SW_SHOW : SW_HIDE);
    layout(g.hwnd);
}

void setOpenEnabled(bool on) { EnableWindow(hOpen, on); }
std::string selectedTransport() {
    int selected = (int)SendMessageW(hTransport, CB_GETCURSEL, 0, 0);
    return selected >= 0 && selected < 4 ? modes[selected] : g.transportMode;
}
void scroll(int code) {
    int position = rightOffset;
    SCROLLINFO info{sizeof info, SIF_TRACKPOS};
    if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) { GetScrollInfo(g.hwnd, SB_VERT, &info); position = info.nTrackPos; }
    else if (code == SB_LINEUP) position -= S(36);
    else if (code == SB_LINEDOWN) position += S(36);
    else if (code == SB_PAGEUP) position -= rightPage;
    else if (code == SB_PAGEDOWN) position += rightPage;
    else if (code == SB_TOP) position = 0;
    else if (code == SB_BOTTOM) position = rightHeight;
    position = std::clamp(position, 0, std::max(0, rightHeight - rightPage));
    if (position != rightOffset) { rightOffset = position; layout(g.hwnd); }
}
bool wheel(POINT point, int delta) {
    if (!PtInRect(&rightViewport, point)) return false;
    wheelRemainder += delta;
    while (wheelRemainder >= WHEEL_DELTA) { scroll(SB_LINEUP); wheelRemainder -= WHEEL_DELTA; }
    while (wheelRemainder <= -WHEEL_DELTA) { scroll(SB_LINEDOWN); wheelRemainder += WHEEL_DELTA; }
    return true;
}
}  // namespace mainwin
