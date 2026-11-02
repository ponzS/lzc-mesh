#pragma once
#include <algorithm>
#include <cmath>

// SendInput divides its 0..65535 range into screen-width buckets. Aim at the
// centre of the requested pixel, not its lower boundary (which can round left).
inline long absolutePixel(double normalized, int extent) {
    if (!std::isfinite(normalized) || extent <= 0) return 0;
    const auto pixel = std::clamp(std::round(normalized * extent), 0.0, double(extent - 1));
    return (long)std::clamp(std::floor((pixel + 0.5) * 65536.0 / extent), 0.0, 65535.0);
}

// SendInput accepts integer 1/120-notch units. Retain the fractional remainder
// instead of truncating every continuous-trackpad event separately. No local
// momentum is generated; all incoming motion (including inertia) is used once.
struct WheelRemainder {
    double value = 0;
    long consume(double delta) {
        if (!std::isfinite(delta) || std::abs(delta) > 1000000) return 0;
        value += delta;
        const double rounded = std::round(value);
        const auto whole = (long)(std::abs(value - rounded) < 1e-9 ? rounded : std::trunc(value));
        value -= whole;
        if (std::abs(value) < 1e-9) value = 0;
        return whole;
    }
};
