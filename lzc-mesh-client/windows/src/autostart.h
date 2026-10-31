#pragma once
// Start at login: HKCU\Software\Microsoft\Windows\CurrentVersion\Run (per user, no admin).
namespace autostart {
bool enabled();
bool set(bool on);  // returns false when the registry write fails
}
