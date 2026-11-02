#pragma once
#include "app.h"

#include <shellapi.h>

#include <string>
#include <vector>

// Shared files panel (Win32 list view + transfers on worker threads).
namespace files {
HWND create(HWND parent, HFONT font);
void setFont(HFONT font);
void layout(int x, int y, int w, int h);
const std::wstring &status();   // last transfer message, painted by the window
bool empty();
void update(const std::vector<SharedFile> &files);
void setStatusText(const std::wstring &text);
void setActive(bool on);
bool accepts(uint64_t ticket);
void pickAndUpload();
void onDrop(HDROP drop);
void downloadSelected();
void deleteSelected();
}
