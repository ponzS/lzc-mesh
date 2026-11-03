#pragma once
#include "json.h"
#include <cstdint>
#include <deque>
#include <string>

// Capture order is part of the input protocol. Merge only adjacent motion;
// never move a later position across a click, key, wheel or release barrier.
class InputQueue {
    struct Frame {
        std::string text;
        double dx = 0, dy = 0;
        uint64_t captured = 0;
        bool motion = false;
    };
    std::deque<Frame> frames;
public:
    bool empty() const { return frames.empty(); }
    size_t size() const { return frames.size(); }
    void clear() { frames.clear(); }
    void event(std::string text) { frames.push_back(Frame{std::move(text)}); }
    void motion(double dx, double dy, uint64_t captured) {
        if (dx == 0 && dy == 0) return;
        if (!frames.empty() && frames.back().motion) {
            frames.back().dx += dx; frames.back().dy += dy;
        } else frames.push_back(Frame{"", dx, dy, captured, true});
    }
    std::string take(const std::string &scope = "") {
        Frame f = std::move(frames.front()); frames.pop_front();
        auto text = f.motion ? JsonOut().s("t", "motion").n("dx", f.dx).n("dy", f.dy).n("_capture_us", (double)f.captured).str() : f.text;
        if (!scope.empty()) {
            // Frames are internally serialized JSON objects. Append an escaped
            // capture ID without reparsing every high-frequency mouse frame.
            const auto binding = JsonOut().s("_control_session", scope).str();
            text.pop_back(); text += "," + binding.substr(1);
        }
        return text;
    }
};

// UI-thread handoff follows the same rule. Action messages have no replacement
// key, and a relay position is replaceable only within its target/session/barrier.
inline std::string transportMotionKey(const Json &event) {
    const auto kind = event["_lan"].str();
    const auto &m = event["message"];
    if (kind == "inject" && m["t"].str() == "move") return "move";
    const auto &wire = m["data"];
    const auto &input = wire["input"];
    if (kind == "hub" && m["t"].str() == "direct_relay" && wire["kind"].str() == "input" && input["event"].type == Json::Null)
        return JsonOut().s("session", input["session"].str()).s("target", input["target"].str()).n("barrier", input["barrier"].num()).str();
    return "";
}
