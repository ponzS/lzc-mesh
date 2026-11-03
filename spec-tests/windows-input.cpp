// Portable regressions for the actual Windows serializers, queues and math.
#include "../lzc-mesh-client/windows/src/input_queue.h"
#include "../lzc-mesh-client/windows/src/pointer_math.h"
#include "../lzc-mesh-client/windows/src/capture_pointer.h"
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
    const std::string scope="capture-\"quoted\"-\\bound";
    queue.motion(1,2,timestamp);queue.event(JsonOut().s("t","key").n("code",64).b("down",true).str());
    const auto scopedMotion=parse(queue.take(scope)),scopedKey=parse(queue.take(scope));
    assert(scopedMotion["_control_session"].str()==scope && scopedMotion["dx"].num()==1);
    assert(scopedKey["_control_session"].str()==scope && scopedKey["code"].num()==64);
    auto relay = parse(R"({"_lan":"hub","message":{"t":"direct_relay","data":{"kind":"input","input":{"session":"s","target":"a","barrier":1}}}})");
    const auto original = transportMotionKey(relay); assert(!original.empty());
    relay.o["message"].o["data"].o["input"].o["barrier"] = parse("2");
    assert(transportMotionKey(relay) != original);
    relay.o["message"].o["data"].o["input"].o["event"] = parse(R"({"t":"button","down":true})");
    assert(transportMotionKey(relay).empty());
    assert(transportMotionKey(parse(R"({"_lan":"inject","message":{"t":"key"}})")).empty());

    SourcePointerGate gate;
    assert(!gate.ready(1));gate.begin(1);assert(!gate.ready(1));
    assert(!gate.parked(0));assert(gate.parked(1));assert(gate.ready(1));
    gate.end();assert(!gate.parked(1));gate.begin(2);
    assert(!gate.parked(1));assert(!gate.ready(2));assert(gate.parked(2));
    assert(gate.ready(2) && !gate.ready(1));
    assert(SourcePointerGate::isParkMarker(SourcePointerGate::parkMarker(2)));
    assert(SourcePointerGate::parkMarker(1)!=SourcePointerGate::parkMarker(2));
    assert(uint64_t(uint32_t(SourcePointerGate::parkMarker(2)))==SourcePointerGate::parkMarker(2));
    assert(!SourcePointerGate::isParkMarker(0x4C5A4D45));
    assert(!SourcePointerGate::isParkMarker(0x54535431));
    // Live fault: cursor remained at the Windows exit (728,1439), not at
    // (1280,720). The old centre subtraction turns a (2,-3) motion into a
    // (-550,+716) jump, repeatedly pinning the Mac to its lower-left corner.
    assert(730-1280==-550 && 1436-720==716);
    assert(capturedPointerDelta(730,728,1)==2);
    assert(capturedPointerDelta(1436,1439,1)==-3);
    assert(capturedPointerDelta(1282,1280,1)==2);
    assert(capturedPointerDelta(718,720,2)==-1);
    assert(capturedPointerDelta(1,0,0)==0);

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
    std::cout << "PASS: capture park/epoch gate, actual-cursor displacement, JSON precision, ordered queues, " << coordinates << " pixel coordinates, fractional wheel accumulation\n";
}
