// A connection owns its queue. Producers never wait for network I/O; overflow
// fails closed rather than silently losing an ordered control message.
#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

class HubOutbox {
public:
    struct Frame { std::string text; std::chrono::steady_clock::time_point queued; };
    bool push(const std::string &text) {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopped_ || frames_.size() >= 1024 || text.size() > maxBytes_ - bytes_) return false;
        frames_.push_back({text, std::chrono::steady_clock::now()}); bytes_ += text.size();
        ready_.notify_one(); return true;
    }
    bool take(Frame &frame) {
        std::unique_lock<std::mutex> lock(mu_);
        ready_.wait(lock, [&] { return stopped_ || !frames_.empty(); });
        if (stopped_) return false;
        frame = std::move(frames_.front()); frames_.pop_front(); bytes_ -= frame.text.size(); return true;
    }
    void stop() {
        std::lock_guard<std::mutex> lock(mu_);
        stopped_ = true; frames_.clear(); bytes_ = 0; ready_.notify_all();
    }
private:
    static constexpr size_t maxBytes_ = 2 * 1024 * 1024;
    std::mutex mu_;
    std::condition_variable ready_;
    std::deque<Frame> frames_;
    size_t bytes_ = 0;
    bool stopped_ = false;
};
