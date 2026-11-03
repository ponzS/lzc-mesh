#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

// An entry relocation must pass our own low-level hook before physical input
// can be forwarded. A late relocation cannot re-arm a canceled/new capture.
class SourcePointerGate {
    std::atomic<uint64_t> state{0};
public:
    void begin(uint64_t epoch) { state.store(epoch << 1); }
    void end() { state.store(0); }
    bool parked(uint64_t epoch) {
        if (!epoch) return false;
        uint64_t expected=epoch << 1;
        return state.compare_exchange_strong(expected,expected | 1);
    }
    bool ready(uint64_t epoch) const { return epoch && state.load()==((epoch << 1) | 1); }
    // The input stack may retain only dwExtraInfo's low 32 bits. Use a
    // distinct positive 32-bit tag, never an epoch stored in the upper half.
    // Only the current pending park can arm; full epochs still guard queues,
    // timers and transport. Old OS events cannot outlive 2^24 ordered parks.
    static constexpr uint32_t marker=0x6D000000;
    static uint32_t parkMarker(uint64_t epoch) { return marker | uint32_t(epoch & 0x00FFFFFF); }
    static bool isParkMarker(uint64_t value) { return (uint32_t(value)&0xFF000000)==marker; }
};

// WH_MOUSE_LL supplies a proposed screen position. Its displacement is relative
// to the actual pre-event cursor, not an assumed centre that a warp may not have
// reached. Keep the existing system-coordinate DPI gain unchanged.
inline double capturedPointerDelta(double proposed,double actual,double scale) {
    if (!std::isfinite(proposed) || !std::isfinite(actual) || !std::isfinite(scale) || scale<=0) return 0;
    return (proposed-actual)/scale;
}
