#pragma once
#include <cstdint>
#include <string>

// Windows input. Low-level hooks on a dedicated thread detect screen edges and
// capture local input while this PC controls another device. Receive-side
// SendInput and repeat timers run on receiver's ordered executor, never the UI.
// Same SendInput technique as
// lightos-remote-desktop's Windows host (absolute virtual-desktop coordinates,
// scan codes, per-monitor DPI awareness).
namespace input {
void start();
void stop();
void refreshScreen();  // re-reads virtual desktop size; returns via g.screen*
void setEdges(bool left, bool right, bool up, bool down);
void setHubConnected(bool on);
void setSharing(bool on);   // tray switch: when off, screen edges are ignored
void capture(bool on, const std::string &session = "", uint64_t receiverRole = 0);
void cancelCapture(); // thread-safe: immediately stop swallowing local input
uint64_t captureGeneration();
void releaseRemote();  // bound to the source capture, including delayed faults
void warp(double nx, double ny);
void enter(double nx, double ny);
void leave();
bool receiving();
uint64_t receiveEpoch();
void completeLocalActivity(uint64_t epoch, bool replay);
void move(double nx, double ny);
void button(int b, bool down);
void wheel(double dx, double dy);
void key(uint32_t evdev, bool down);
bool isElevated();
}
