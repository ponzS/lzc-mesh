// Production receiver.cpp + input.cpp on an isolated test window. No peer,
// network, hooks, pairing, permissions or production-client lifecycle changes.
// Run only in an unlocked interactive session with the production Windows
// client dormant and no control session involving Windows. --ui-queue
// demonstrates the old starvation path.
#include "../lzc-mesh-client/windows/src/app.h"
#include "../lzc-mesh-client/windows/src/input.h"
#include "../lzc-mesh-client/windows/src/receiver.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <thread>

App g;
std::wstring fmt(const wchar_t *format, ...) { wchar_t b[1024]; va_list a; va_start(a,format); vswprintf(b,1024,format,a); va_end(a); return b; }
void logLine(const std::wstring &) {}
namespace direct { uint64_t nowUs() { return GetTickCount64()*1000; } void send(const std::string &) {} void applied(const Json &) {} }
namespace {
constexpr UINT prepareMessage=WM_APP+100, inputMessage=WM_APP+101;
HWND window, oldForeground;
POINT oldCursor{};
HANDLE prepared;
std::atomic<bool> done{false}, tracking{false}, passed{false};
std::atomic<int> commands{0}, prepares{0};
std::atomic<unsigned> lastCommand{0};
std::atomic<int> lastHit{0};
POINT requested{}, actual{};
std::atomic<int> actualHit{0}, downHit{0}, repeatDowns{0}, repeatUps{0};
bool uiQueue=false;
std::string resultPath;
std::atomic<bool> legacyPath{false}, disconnectReleased{false};
Json parse(const std::string &s) { Json v; if(!Json::parse(s,v)) ExitProcess(3); return v; }
void result(bool passed,const char *reason) {
    std::ofstream out(resultPath); out<<JsonOut().b("passed",passed).s("error",reason).b("ui_queue",uiQueue)
        .b("disconnect_released",disconnectReleased).n("system_commands",commands.load()).n("last_command",lastCommand.load()).n("last_hit",lastHit.load()).n("requested_x",requested.x).n("requested_y",requested.y).n("actual_x",actual.x).n("actual_y",actual.y).n("actual_hit",actualHit.load()).n("down_hit",downHit.load()).n("repeat_downs",repeatDowns.load()).n("repeat_ups",repeatUps.load()).b("iconic",IsIconic(window)).b("zoomed",IsZoomed(window)).b("visible",IsWindowVisible(window)).n("rounds",prepares.load()).s("evidence","production receiver/input, real OS caption clicks, synthetic input; no network or physical hardware certification").str();
}
bool unlocked() {
    HDESK desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS); wchar_t name[64]{}; DWORD size=0;
    bool ok=desktop&&GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof name,&size)&&wcscmp(name,L"Default")==0;
    if(desktop) CloseDesktop(desktop); return ok;
}
void emit(const Json &value) {
    if(uiQueue) { auto *p=new Json(value); if(!PostMessageW(window,inputMessage,0,(LPARAM)p))delete p; }
    else if (legacyPath) { Json legacy=value;legacy.o.erase("_control_session");receiver::hub(legacy,1); }
    else receiver::native(value,1);
}
void emit(const char *text) { emit(parse(text)); }
bool pointFor(int hit,POINT &point) {
    TITLEBARINFOEX info{};info.cbSize=sizeof info;DWORD_PTR status=0;
    if(SendMessageTimeoutW(window,WM_GETTITLEBARINFOEX,0,(LPARAM)&info,SMTO_ABORTIFHUNG,300,&status)) {
        const RECT button=info.rgrect[hit==HTMINBUTTON?2:hit==HTMAXBUTTON?3:5];
        if(button.right>button.left&&button.bottom>button.top){point=POINT{(button.left+button.right)/2,(button.top+button.bottom)/2};return true;}
    }
    RECT r;GetWindowRect(window,&r);
    for(int y=r.top+GetSystemMetrics(SM_CYFRAME)+GetSystemMetrics(SM_CYCAPTION)/2;y<std::min((LONG)(r.top+GetSystemMetrics(SM_CYCAPTION)+20),r.bottom);y+=3)
        for(int x=r.right-10;x>r.left+30;x-=3) {
            DWORD_PTR found=0;
            if(SendMessageTimeoutW(window,WM_NCHITTEST,0,MAKELPARAM(x,y),SMTO_ABORTIFHUNG,300,&found)&&int(found)==hit){point=POINT{x,y};return true;}
        }
    return false;
}
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m==prepareMessage) {
        ShowWindow(h,SW_RESTORE);SetWindowPos(h,HWND_TOPMOST,120,100,900,650,SWP_SHOWWINDOW);SetForegroundWindow(h);SetFocus(h);
        ++prepares;SetEvent(prepared);return 0;
    }
    if(m==inputMessage) { auto *p=(Json*)l;receiver::native(*p,1);delete p;return 0; }
    if(m==WM_APP_RECEIVER) {delete (receiver::Notice*)l;return 0;}
    if(m==WM_NCLBUTTONDOWN) {downHit=(int)w;tracking=true;auto r=DefWindowProcW(h,m,w,l);tracking=false;return r;}
    if(m==WM_SYSCOMMAND && ((w & 0xfff0)==SC_MINIMIZE || (w & 0xfff0)==SC_MAXIMIZE || (w & 0xfff0)==SC_CLOSE)) {lastCommand=(unsigned)w; ++commands;}
    if((m==WM_KEYDOWN||m==WM_KEYUP)&&w==VK_F6){if(m==WM_KEYDOWN)++repeatDowns;else ++repeatUps;return 0;}
    if(m==WM_CLOSE) {ShowWindow(h,SW_HIDE);return 0;}
    if(m==WM_DESTROY) {PostQuitMessage(0);return 0;}
    if(m==WM_PAINT) {PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));EndPaint(h,&ps);return 0;}
    return DefWindowProcW(h,m,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!unlocked())return 3;
    uiQueue=wcsstr(GetCommandLineW(),L"--ui-queue")!=nullptr;
    char home[MAX_PATH];GetEnvironmentVariableA("USERPROFILE",home,MAX_PATH);
    resultPath=std::string(home)+"\\mesh-parity\\caption-os"+(uiQueue?"-before":"-after")+".json";
    GetCursorPos(&oldCursor);oldForeground=GetForegroundWindow();
    WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=instance;wc.lpszClassName=L"LzcMeshCaptionRegression";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"LZC Mesh isolated caption verification",WS_OVERLAPPEDWINDOW,120,100,900,650,nullptr,nullptr,instance,nullptr);
    g.hwnd=window;g.deviceId="caption-fixture";input::refreshScreen();
    receiver::start();receiver::bindHub(1);receiver::bindNative(1);receiver::setEnabled(true);
    receiver::hub(parse(R"({"t":"state","devices":[{"id":"caption-fixture","enabled":true,"trusted":true}],"settings":{"input_enabled":true},"active":{"control_id":"caption"}})"),1);
    emit(R"({"t":"direct_active","source":"fixture","target":"caption-fixture","_control_session":"caption"})");
    emit(R"({"t":"enter","x":0.5,"y":0.5,"_control_session":"caption"})");
    prepared=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread watchdog([]{for(int i=0;i<240&&!done;i++)Sleep(100);if(!done){input::button(1,false);Sleep(100);result(false,"caption tracking starved button-up / timed out");ExitProcess(2);}});
    std::thread controller([]{
        bool ok=true;const char *reason="";
        for(int round=0;round<(uiQueue?3:6)&&ok;round++)for(int hit:{HTMINBUTTON,HTMAXBUTTON,HTCLOSE}) {
            // Repeat the same real caption clicks through authenticated legacy
            // hub dispatch, not only through native transport callbacks.
            legacyPath=round>=3;
            if(round==3&&hit==HTMINBUTTON) {
                receiver::hub(parse(R"({"t":"enter","x":0.5,"y":0.5})"),1);Sleep(50);
            }
            ResetEvent(prepared);PostMessageW(window,prepareMessage,0,0);
            if(WaitForSingleObject(prepared,2000)!=WAIT_OBJECT_0){ok=false;reason="fixture prepare timed out";break;}
            Sleep(150);
            if(!unlocked()||GetForegroundWindow()!=window){ok=false;reason="fixture lost focus or secure desktop";break;}
            lastHit=hit;
            POINT p{};if(!pointFor(hit,p)){ok=false;reason="caption hit target missing";break;}
            emit(parse(JsonOut().s("t","move").n("x",double(p.x-g.screenX)/g.screenW).n("y",double(p.y-g.screenY)/g.screenH).s("_control_session","caption").str()));
            Sleep(80);requested=p;GetCursorPos(&actual);
            DWORD_PTR actualResult=0;SendMessageTimeoutW(window,WM_NCHITTEST,0,MAKELPARAM(actual.x,actual.y),SMTO_ABORTIFHUNG,300,&actualResult);actualHit=(int)actualResult;
            int before=commands;
            emit(R"({"t":"button","button":1,"down":true,"_control_session":"caption"})");
            for(int i=0;i<100&&!tracking;i++)Sleep(5);
            if(!tracking){ok=false;reason="real NC_DOWN was not observed";break;}
            emit(R"({"t":"button","button":1,"down":false,"_control_session":"caption"})");
            for(int i=0;i<200&&commands==before;i++)Sleep(5);
            if(commands==before){ok=false;reason="remote button-up did not complete caption command";break;}
            for(int i=0;i<100&&tracking;i++)Sleep(5);
            if(tracking){ok=false;reason="caption tracking did not finish";break;}
            auto completed=[=]{return hit==HTMINBUTTON?IsIconic(window):hit==HTMAXBUTTON?IsZoomed(window):!IsWindowVisible(window);};
            for(int i=0;i<200&&!completed();i++)Sleep(5);
            if(!completed()){ok=false;reason="caption command did not reach its visible result";break;}
            if(GetAsyncKeyState(VK_LBUTTON)&0x8000){ok=false;reason="button left held after caption command";break;}
        }
        if(ok) {
            ResetEvent(prepared);PostMessageW(window,prepareMessage,0,0);
            if(WaitForSingleObject(prepared,2000)!=WAIT_OBJECT_0){ok=false;reason="repeat fixture prepare timed out";}
            if(ok) {
                emit(R"({"t":"key","code":64,"down":true,"_control_session":"caption"})");
                for(int i=0;i<300&&repeatDowns<2;i++)Sleep(5);
                emit(R"({"t":"key","code":64,"down":false,"_control_session":"caption"})");
                for(int i=0;i<100&&repeatUps<1;i++)Sleep(5);
                if(repeatDowns<2||repeatUps!=1){ok=false;reason="executor key-repeat timer or key-up did not run";}
                const auto count=repeatDowns.load();Sleep(200);
                if(repeatDowns!=count){ok=false;reason="repeat continued after key-up";}
            }
        }
        if(ok) {
            // Exercise OS-held state cleanup without relying on the UI. Use
            // only this fixture's client area, never a user's application.
            RECT r;GetWindowRect(window,&r);
            emit(parse(JsonOut().s("t","move").n("x",double((r.left+r.right)/2-g.screenX)/g.screenW).n("y",double((r.top+r.bottom)/2-g.screenY)/g.screenH).s("_control_session","caption").str()));
            emit(R"({"t":"key","code":64,"down":true,"_control_session":"caption"})");
            emit(R"({"t":"button","button":1,"down":true,"_control_session":"caption"})");
            for(int i=0;i<100&&(!(GetAsyncKeyState(VK_F6)&0x8000)||!(GetAsyncKeyState(VK_LBUTTON)&0x8000));i++)Sleep(5);
            if(!(GetAsyncKeyState(VK_F6)&0x8000)||!(GetAsyncKeyState(VK_LBUTTON)&0x8000)){ok=false;reason="cleanup fixture never held test key/button";}
            receiver::closeHub(1);
            for(int i=0;i<200&&(input::receiving()||(GetAsyncKeyState(VK_F6)&0x8000)||(GetAsyncKeyState(VK_LBUTTON)&0x8000));i++)Sleep(5);
            disconnectReleased=!input::receiving()&&!(GetAsyncKeyState(VK_F6)&0x8000)&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000);
            if(!disconnectReleased){ok=false;reason="disconnect left OS key/button held";}
        }
        // On the old path the up is stuck in the UI queue. Bypass that queue
        // only for fixture cleanup so this negative control leaves no held input.
        if (!ok) receiver::native(parse(R"({"t":"button","button":1,"down":false,"_control_session":"caption"})"),1);
        receiver::closeHub(1);Sleep(100);
        passed=ok;result(ok,reason);done=true;PostThreadMessageW(GetWindowThreadProcessId(window,nullptr),WM_QUIT,0,0);
    });
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    controller.join();receiver::shutdown();done=true;watchdog.join();CloseHandle(prepared);
    if(unlocked()&&(GetForegroundWindow()==window||!IsWindowVisible(window)))SetCursorPos(oldCursor.x,oldCursor.y);
    DestroyWindow(window);if(IsWindow(oldForeground))SetForegroundWindow(oldForeground);
    return passed?0:1;
}
