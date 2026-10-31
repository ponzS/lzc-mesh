#pragma once
#include <string>

// Shared clipboard (text). Listens with AddClipboardFormatListener on the main
// window and writes text that arrives from other devices.
namespace clipboard {
void start(HWND hwnd);          // register the listener
void onUpdate();                // WM_CLIPBOARDUPDATE
void set(const std::string &utf8, const std::string &from);
}
