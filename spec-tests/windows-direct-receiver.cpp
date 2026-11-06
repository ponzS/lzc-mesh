// Real direct.cpp -> Rust routing -> receiver.cpp, no UI pump, OS input or
// external network. Only OS injection, Bluetooth and hub writes are observed.
#undef NDEBUG
#include "../lzc-mesh-client/windows/src/app.h"
#include "../lzc-mesh-client/windows/src/direct.h"
#include "../lzc-mesh-client/windows/src/receiver.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
App g;
namespace {
std::atomic<bool> receiving{false};
std::atomic<int> downs{0},ups{0},acks{0};
DWORD controller;
void offUI() { assert(GetCurrentThreadId()!=controller); }
Json parse(const std::string &s) {Json v;assert(Json::parse(s,v));return v;}
void wait(const std::function<bool()> &predicate) {for(int i=0;i<3000&&!predicate();i++)Sleep(1);assert(predicate());}
void state(bool on) {
    auto text=std::string(R"({"t":"state","devices":[{"id":"fixture","x":0,"y":0,"w":1000,"h":800,"online":true,"enabled":true,"trusted":true},{"id":"peer","x":1000,"y":0,"w":1000,"h":800,"online":true,"enabled":true,"trusted":true}],"settings":{"transport_mode":"relay","input_enabled":)")+(on?"true":"false")+R"(},"active":{"source":"peer","target":"fixture","control_id":"session"}})";
    receiver::hub(parse(text),1);assert(direct::network(text));
}
void action(int barrier,const char *event) {
    auto text=std::string(R"({"t":"direct_relay","from":"peer","data":{"kind":"input","input":{"session":"session","source":"peer","target":"fixture","seq":)")+std::to_string(barrier)+R"(,"barrier":)"+std::to_string(barrier)+R"(,"x":100,"y":100,"event":)"+event+"}}}";
    assert(direct::network(text));
}
}
namespace input {
void cancelCapture() {}
void leave() {offUI();receiving=false;}
void enter(double,double) {offUI();receiving=true;}
bool receiving(){return ::receiving;}
uint64_t receiveEpoch(){return 1;}
void completeLocalActivity(uint64_t,bool){offUI();}
void warp(double,double){offUI();}
void move(double,double){offUI();}
void button(int,bool down){offUI();if(down)++downs;else ++ups;}
void key(uint32_t,bool down){offUI();if(down)++downs;else ++ups;}
void wheel(double,double){offUI();}
}
namespace bluetooth {void bind(uint64_t){}void event(const Json &,uint64_t){}void stop(){}void shutdown(){}}
namespace hubws {
unsigned generation(){return 1;}
void send(const std::string &text){auto j=parse(text);if(j["t"].str()=="direct_relay")++acks;}
void sendFor(unsigned generation,const std::string &text){assert(generation==1);send(text);}
}
std::wstring widen(const std::string &s){return std::wstring(s.begin(),s.end());}
void logLine(const std::wstring &){}
void setStatus(int,Level,const std::wstring &){}
int main(){
    controller=GetCurrentThreadId();g.deviceId="fixture";
    receiver::start();receiver::bindHub(1);receiver::setEnabled(true);direct::connected();state(true);
    assert(direct::network(R"({"t":"direct_session","session":"session","source":"peer","target":"fixture","x":100,"y":100})"));
    action(1,R"({"t":"enter"})");wait([]{return bool(receiving);});
    action(2,R"({"t":"button","button":1,"down":true})");wait([]{return downs==1;});
    // Delayed up models a click arriving after UI enters caption tracking.
    Sleep(100);action(3,R"({"t":"button","button":1,"down":false})");wait([]{return ups==1;});
    action(4,R"({"t":"key","code":64,"down":true})");action(5,R"({"t":"key","code":64,"down":false})");
    wait([]{return downs==2&&ups==2&&acks>=5;});
    state(false);wait([]{return !receiving;});
    action(6,R"({"t":"button","button":1,"down":true})");Sleep(50);assert(downs==2);
    direct::shutdown();receiver::shutdown();
    puts("PASS: production Windows direct adapter + Rust core + receiver, relay enter/delayed button-up/key/acks and pause without any UI pumping");
}
