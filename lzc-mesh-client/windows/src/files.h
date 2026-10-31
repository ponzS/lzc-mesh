#pragma once
#include "app.h"

#include <shellapi.h>

#include <string>
#include <vector>

// Shared files panel (Win32 list view + transfers on worker threads).
namespace files {
HWND create(HWND parent, HFONT font);
HWND statusLabel();
void layout(int x, int y, int w, int h, int statusH);
void update(const std::vector<SharedFile> &files);
void setStatusText(const std::wstring &text);
void pickAndUpload();
void onDrop(HDROP drop);
void downloadSelected();
void deleteSelected();
}
