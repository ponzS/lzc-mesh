#include "app.h"
#include "clipboard.h"

#include "net.h"

namespace clipboard {
namespace {
HWND owner;
std::wstring lastText;
const size_t kMaxBytes = 256 * 1024;
UINT fmtExclude, fmtCanHistory, fmtCanCloud;

// Password managers mark secrets with these formats (Windows clipboard history
// convention); such content must never leave the machine.
bool isSecret() {
    if (IsClipboardFormatAvailable(fmtExclude)) return true;
    for (UINT f : {fmtCanHistory, fmtCanCloud}) {
        if (!IsClipboardFormatAvailable(f)) continue;
        HANDLE h = GetClipboardData(f);
        if (DWORD *v = h ? (DWORD *)GlobalLock(h) : nullptr) {
            bool deny = *v == 0;
            GlobalUnlock(h);
            if (deny) return true;
        }
    }
    return false;
}

bool openClipboard() {
    // Another process may hold the clipboard briefly; retry a few times.
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(20);
    }
    return false;
}
}  // namespace

void start(HWND hwnd) {
    owner = hwnd;
    fmtExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    fmtCanHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    fmtCanCloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    AddClipboardFormatListener(hwnd);
}

void onUpdate() {
    if (!g.clipboardEnabled || !g.hubConnected) return;
    if (GetClipboardOwner() == owner) return;  // our own write
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !openClipboard()) return;
    std::wstring text;
    bool secret = isSecret();
    if (!secret) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t *p = (const wchar_t *)GlobalLock(h)) {
                text = p;
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    if (secret || text.empty() || text == lastText) return;
    std::string utf8 = narrow(text);
    if (utf8.size() > kMaxBytes) {
        logLine(L"剪贴板文本超过 256 KB，未同步");
        return;
    }
    lastText = text;
    hubws::send(JsonOut().s("t", "clipboard").s("text", utf8).str());
    logLine(fmt(L"已同步本机剪贴板（%zu 字节）", utf8.size()));
}

void set(const std::string &utf8, const std::string &from) {
    if (!g.clipboardEnabled || utf8.empty()) return;
    std::wstring text = widen(utf8);
    if (text == lastText) return;
    if (!openClipboard()) {
        logLine(L"写入剪贴板失败：剪贴板被其他程序占用");
        return;
    }
    EmptyClipboard();
    // Windows wants CRLF line endings in CF_UNICODETEXT.
    std::wstring crlf;
    for (wchar_t c : text) {
        if (c == L'\n' && (crlf.empty() || crlf.back() != L'\r')) crlf += L'\r';
        crlf += c;
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (crlf.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), crlf.c_str(), (crlf.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    lastText = crlf;
    logLine(fmt(L"已接收来自 %ls 的剪贴板（%zu 字节）", from.empty() ? L"其他设备" : widen(from).c_str(), utf8.size()));
}
}  // namespace clipboard
