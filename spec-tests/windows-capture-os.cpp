// Isolated OS-level test of the production Windows capture implementation.
// Link input.cpp unchanged; only transport/logging are substituted. Never sends
// network input, reads keyboard text, changes permissions, or targets a peer.
// Opt-in maintenance tool: the external controller MUST verify account-wide
// input pause and no active session first. Briefly stops/restarts the installed
// Windows client to prevent the two copies' global hooks from interfering.
#include "../lzc-mesh-client/windows/src/app.h"
#include "../lzc-mesh-client/windows/src/input.h"
#include "../lzc-mesh-client/windows/src/capture_pointer.h"
#include <cstdarg>
#include <cmath>
#include <fstream>
#include <chrono>
#include <thread>

App g;
std::mutex framesMutex;
std::vector<Json> frames;
std::vector<std::wstring> captureLogs;
std::wstring fmt(const wchar_t *format,...) {wchar_t text[1024];va_list args;va_start(args,format);vswprintf(text,1024,format,args);va_end(args);return text;}
void logLine(const std::wstring &text) {captureLogs.push_back(text);}
namespace direct {
uint64_t nowUs() {return uint64_t(GetTickCount64())*1000;}
void send(const std::string &text) {Json v;if(Json::parse(text,v)){std::lock_guard<std::mutex> lock(framesMutex);frames.push_back(v);}}
}
namespace {
struct NativeLifecycle {
    std::wstring executable;
    bool restart=false;
    bool stop() {
        wchar_t local[MAX_PATH];if(!GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH))return false;
        executable=std::wstring(local)+L"\\Programs\\LZC Mesh\\LZC Mesh.exe";
        HWND client=FindWindowW(L"LzcMeshMain",nullptr);
        if(client)PostMessageW(client,RegisterWindowMessageW(L"LzcMeshQuit"),0,0);
        for(int i=0;i<100&&FindWindowW(L"LzcMeshMain",nullptr);i++)Sleep(50);
        if(FindWindowW(L"LzcMeshMain",nullptr))return false;
        restart=true;Sleep(100);return true;
    }
    ~NativeLifecycle() {
        if(!restart)return;
        std::wstring command=L"\""+executable+L"\" --background";
        STARTUPINFOW info{sizeof info};PROCESS_INFORMATION process{};
        if(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&info,&process)) {CloseHandle(process.hThread);CloseHandle(process.hProcess);}
    }
};
HWND window,previousForeground;POINT original{},anchor{};
int phase=0,completedRounds=0;DWORD due=0,started=0;bool running=false;std::string failure,path;
std::vector<std::string> results;
bool unlocked() {
    HDESK desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);wchar_t name[64]{};DWORD needed=0;
    const bool ok=desktop&&GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof name,&needed)&&wcscmp(name,L"Default")==0;
    if(desktop)CloseDesktop(desktop);return ok;
}
void fail(const char *why) {failure=why;PostQuitMessage(1);}
bool ownFocus() {if(!unlocked()||GetForegroundWindow()!=window){fail("Private fixture not focused or secure desktop");return false;}return true;}
void clearFrames() {std::lock_guard<std::mutex> lock(framesMutex);frames.clear();}
bool checkDelta(double dx,double dy) {
    std::lock_guard<std::mutex> lock(framesMutex);double x=0,y=0;int n=0;
    for(const auto &frame:frames)if(frame["t"].str()=="motion") {
        if(frame["_control_session"].str()!="os-capture-"+std::to_string(completedRounds))return false;
        x+=frame["dx"].num();y+=frame["dy"].num();n++;
    }
    return n>0&&std::abs(x-dx/g.dpiScale)<.01&&std::abs(y-dy/g.dpiScale)<.01;
}
void mouseAt(POINT p,ULONG_PTR marker) {
    if(!ownFocus())return;
    INPUT in{};in.type=INPUT_MOUSE;in.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx=(LONG)std::ceil((p.x-g.screenX)*65536.0/g.screenW);
    in.mi.dy=(LONG)std::ceil((p.y-g.screenY)*65536.0/g.screenH);in.mi.dwExtraInfo=marker;
    if(SendInput(1,&in,sizeof in)!=1)fail("Synthetic mouse refused");
}
bool parked() {POINT actual{};return GetCursorPos(&actual)&&actual.x==anchor.x&&actual.y==anchor.y;}
void tick() {
    if(GetTickCount()-started>12000){fail("Capture OS test timed out");return;}
    if((int32_t)(GetTickCount()-due)<0)return;
    if(!ownFocus())return;
    const POINT edge{g.screenX+int(g.screenW*(.2+.3*completedRounds)),g.screenY+g.screenH-1};
    switch(phase) {
    case 0:
        input::move(double(edge.x-g.screenX)/g.screenW,double(edge.y-g.screenY)/g.screenH);
        phase=1;due=GetTickCount()+80;break;
    case 1:
        clearFrames();input::capture(true,"os-capture-"+std::to_string(completedRounds));
        phase=2;due=GetTickCount()+350;break;
    case 2:
        if(!parked()){fail("Marked capture did not park at centre");return;}
        mouseAt(POINT{anchor.x+2,anchor.y+3},0x54535431);
        phase=3;due=GetTickCount()+100;break;
    case 3:
        if(!checkDelta(2,3)||!parked()){fail("Positive small displacement wrong or cursor moved locally");return;}
        clearFrames();mouseAt(POINT{anchor.x-2,anchor.y-3},0x54535431);
        phase=4;due=GetTickCount()+100;break;
    case 4:
        if(!checkDelta(-2,-3)||!parked()){fail("Negative small displacement wrong or cursor moved locally");return;}
        clearFrames();mouseAt(edge,(ULONG_PTR)SourcePointerGate::parkMarker(input::captureGeneration()-1));
        phase=5;due=GetTickCount()+80;break;
    case 5:
        if(!parked()){fail("Stale park moved a fresh capture");return;}
        input::move(.1,.9);phase=6;due=GetTickCount()+80;break;
    case 6:
        if(!parked()){fail("Queued old receiver move displaced source");return;}
        {std::lock_guard<std::mutex> lock(framesMutex);if(!frames.empty()){fail("Old OS move reached source transport");return;}}
        results.push_back(JsonOut().n("round",completedRounds+1).b("marked_park",true).b("signed_small_motion",true).b("source_session_binding",true).b("stale_os_motion_rejected",true).str());
        input::capture(false);completedRounds++;phase=0;due=GetTickCount()+80;
        if(completedRounds==3)PostQuitMessage(0);break;
    }
}
LRESULT CALLBACK proc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_TIMER){tick();return 0;}
    if(message==WM_APP_INPUT) {
        if(w==0||w==2||w==3) {
            POINT p{};GetCursorPos(&p);
            failure="Production capture status="+std::to_string(w)+" phase="+std::to_string(phase)+" param="+std::to_string(l)+" cursor="+std::to_string(p.x)+","+std::to_string(p.y)+" anchor="+std::to_string(anchor.x)+","+std::to_string(anchor.y);
            PostQuitMessage(1);
        }
        return 0;
    }
    if(message==WM_CLOSE){fail("Fixture closed");return 0;}
    if(message==WM_PAINT){PAINTSTRUCT p;HDC dc=BeginPaint(h,&p);RECT r;GetClientRect(h,&r);FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));EndPaint(h,&p);return 0;}
    return DefWindowProcW(h,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!unlocked())return 3;
    char home[MAX_PATH];if(!GetEnvironmentVariableA("USERPROFILE",home,MAX_PATH))return 2;
    path=std::string(home)+"\\mesh-parity\\capture-os.json";
    NativeLifecycle production;
    if(!production.stop())return 4;
    GetCursorPos(&original);previousForeground=GetForegroundWindow();
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=proc;wc.lpszClassName=L"LzcMeshPrivateCaptureOS";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"LZC Mesh isolated capture verification",WS_POPUP|WS_VISIBLE,GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),nullptr,nullptr,instance,nullptr);
    g.hwnd=window;SetForegroundWindow(window);SetFocus(window);
    MONITORINFO info{sizeof info};GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&info);
    anchor=POINT{(info.rcMonitor.left+info.rcMonitor.right)/2,(info.rcMonitor.top+info.rcMonitor.bottom)/2};
    input::start();running=true;started=GetTickCount();SetTimer(window,1,20,nullptr);
    MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    input::stop();running=false;
    if(unlocked()&&GetForegroundWindow()==window){SetCursorPos(original.x,original.y);Sleep(20);}
    DestroyWindow(window);if(IsWindow(previousForeground))SetForegroundWindow(previousForeground);
    std::string list="[";for(const auto &result:results){if(list.size()>1)list+=',';list+=result;}list+=']';Json items;Json::parse(list,items);
    std::ofstream diagnostic(path+".log");
    for(const auto &text:captureLogs){for(wchar_t ch:text)diagnostic<<(ch<128?char(ch):'?');diagnostic<<'\n';}
    std::ofstream out(path);out<<JsonOut().s("error",failure).b("passed",failure.empty()&&completedRounds==3).raw("results",items).s("evidence","production input.cpp on real Windows, synthetic OS mouse; no peer or physical hardware certification").str();
    return failure.empty()&&completedRounds==3?0:1;
}
