// Portable regressions for the actual Windows serializers, queues and math.
#include "../lzc-mesh-client/windows/src/input_queue.h"
#include "../lzc-mesh-client/windows/src/pointer_math.h"
#include <cassert>
#include <iostream>
#include <limits>

static Json parse(const std::string &s) { Json v; assert(Json::parse(s, v)); return v; }
int main() {
    const auto timestamp = 123456789012345ULL;
    const double coordinate = 0.12345678901234567;
    auto v = parse(JsonOut().n("timestamp", double(timestamp)).n("x", coordinate).str());
    assert(v["timestamp"].num() == double(timestamp));
    assert(v["x"].num() == coordinate);
    auto nested = parse(JsonOut().s("name", "中文 😀\n").raw("trace", v).str());
    assert(parse(nested.dump())["trace"]["timestamp"].num() == double(timestamp));
    for (const auto *s : {"{} garbage", "nan", "01", "1e999", "-", "1.", "[true,]"}) { Json bad; assert(!Json::parse(s, bad)); }

    InputQueue queue;
    queue.motion(0.25, 0, timestamp); queue.motion(0.5, 0, timestamp + 1);
    queue.event(JsonOut().s("t", "button").b("down", true).str());
    queue.motion(5, 2, timestamp + 2);
    queue.event(JsonOut().s("t", "button").b("down", false).str());
    assert(queue.size() == 4);
    auto first = parse(queue.take()); assert(first["dx"].num() == 0.75 && first["_capture_us"].num() == double(timestamp));
    assert(parse(queue.take())["down"].boolean());
    assert(parse(queue.take())["dx"].num() == 5);
    assert(!parse(queue.take())["down"].boolean()); assert(queue.empty());
    auto relay = parse(R"({"_lan":"hub","message":{"t":"direct_relay","data":{"kind":"input","input":{"session":"s","target":"a","barrier":1}}}})");
    const auto original = transportMotionKey(relay); assert(!original.empty());
    relay.o["message"].o["data"].o["input"].o["barrier"] = parse("2");
    assert(transportMotionKey(relay) != original);
    relay.o["message"].o["data"].o["input"].o["event"] = parse(R"({"t":"button","down":true})");
    assert(transportMotionKey(relay).empty());
    assert(transportMotionKey(parse(R"({"_lan":"inject","message":{"t":"key"}})")).empty());

    int coordinates = 0;
    for (int extent : {800, 1080, 1440, 1920, 2560, 3840, 7680}) {
        for (int pixel = 0; pixel < extent; pixel++) {
            const auto absolute = absolutePixel(double(pixel) / extent, extent);
            assert(absolute >= 0 && absolute <= 65535);
            assert(absolute * extent / 65536 == pixel); coordinates++;
        }
        assert(absolutePixel(-1, extent) * extent / 65536 == 0);
        assert(absolutePixel(2, extent) * extent / 65536 == extent - 1);
    }
    for (double unit : {0.25, 0.1, -0.25, -0.1}) {
        WheelRemainder remainder; long sum = 0;
        for (int n = 0; n < 1200; n++) sum += remainder.consume(unit);
        assert(sum == std::lround(unit * 1200)); assert(std::abs(remainder.value) < 1e-8);
    }
    WheelRemainder remainder;
    assert(remainder.consume(0.25) == 0);
    assert(remainder.consume(std::numeric_limits<double>::infinity()) == 0);
    assert(remainder.consume(0.75) == 1);
    std::cout << "PASS: JSON precision, ordered queues, " << coordinates << " pixel coordinates, fractional wheel accumulation\n";
}
