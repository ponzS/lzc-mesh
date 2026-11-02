// An isolated full-screen test target; records only input delivered to this
// window. Commands are read from its private test directory, never a network.
#include <windows.h>
#include <windowsx.h>
#include "../lzc-mesh-client/windows/src/json.h"
#include <fstream>
#include <string>
#include <vector>

namespace {
HWND window, previousForeground;
POINT previousCursor{};
std::string statusPath;
std::vector<std::string> actions, positions;
long wheelX=0,wheelY=0;
int moves=0,commandId=0;
DWORD started;
std::string commandError;
constexpr int MODE=110, PAUSE=111, CLIENT=112;
void record(std::string value) { if(actions.size()<2048) actions.push_back(std::move(value)); }
void mouseAction(const char *kind, int button, bool down, LPARAM pos) {
    record(JsonOut().s("kind",kind).n("button",button).b("down",down).n("x",GET_X_LPARAM(pos)).n("y",GET_Y_LPARAM(pos)).str());
}
void snapshot() {
    POINT p{};GetCursorPos(&p);
    HWND mesh=FindWindowW(L"LzcMeshMain",nullptr);
    std::string list="[";
    for(const auto &s:actions){if(list.size()>1)list+=',';list+=s;}list+=']';
    Json events;Json::parse(list,events);
    std::string motion="[";
    for(const auto &s:positions){if(motion.size()>1)motion+=',';motion+=s;}motion+=']';
    Json points;Json::parse(motion,points);
    auto text=JsonOut().n("command",commandId).s("error",commandError).n("cursor_x",p.x).n("cursor_y",p.y)
        .n("moves",moves).n("wheel_x",wheelX).n("wheel_y",wheelY).raw("events",events).raw("positions",points)
        .b("held_a", (GetAsyncKeyState('A') & 0x8000) != 0).b("held_button", (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
        .b("mesh_visible",mesh&&IsWindowVisible(mesh)).b("client_enabled",mesh&&GetPropW(GetDlgItem(mesh,CLIENT),L"on")!=nullptr).n("mode_index",mesh?(double)SendDlgItemMessageW(mesh,MODE,CB_GETCURSEL,0,0):-1).str();
    const auto temp=statusPath+".tmp";
    {std::ofstream out(temp);out<<text;}
    MoveFileExA(temp.c_str(),statusPath.c_str(),MOVEFILE_REPLACE_EXISTING);
}
bool sendKey(WORD scan,bool down,bool ext=false) {
    INPUT in{};in.type=INPUT_KEYBOARD;in.ki.wScan=scan;
    in.ki.dwFlags=KEYEVENTF_SCANCODE|(down?0:KEYEVENTF_KEYUP)|(ext?KEYEVENTF_EXTENDEDKEY:0);in.ki.dwExtraInfo=0x54535431;
    return SendInput(1,&in,sizeof in)==1;
}
void command(const Json &c) {
    commandId=(int)c["id"].num();commandError.clear();
    const auto op=c["op"].str();
    HWND mesh=FindWindowW(L"LzcMeshMain",nullptr);
    if(op=="reset") {actions.clear();positions.clear();moves=0;wheelX=wheelY=0;SetForegroundWindow(window);}
    else if(op=="quit") {DestroyWindow(window);return;}
    else if(op=="mode") {
        if(!mesh) {commandError="Mesh window unavailable";return;}
        SendDlgItemMessageW(mesh,MODE,CB_SETCURSEL,(WPARAM)c["index"].num(),0);
        SendMessageW(mesh,WM_COMMAND,MAKEWPARAM(MODE,CBN_SELCHANGE),(LPARAM)GetDlgItem(mesh,MODE));
    } else if(op=="client") {
        if(!mesh) {commandError="Mesh window unavailable";return;}
        SendMessageW(mesh,WM_COMMAND,CLIENT,0);
    } else if(op=="pause") {
        if(!mesh) {commandError="Mesh window unavailable";return;}
        SendMessageW(mesh,WM_COMMAND,PAUSE,0);
    } else if(op=="edge") {
        if(!mesh) {commandError="Mesh window unavailable";return;}
        // Same UI message as the real native edge hook; all hub/session checks
        // and subsequent capture go through the installed production client.
        PostMessageW(mesh,WM_APP+5,(WPARAM)c["direction"].num(),(LPARAM)(c["position"].num(0.5)*10000));
    } else if(op=="input") {
        for(const auto &e:c["events"].a) {
            const auto kind=e["t"].str();
            bool ok=true;
            if(kind=="key") ok=sendKey((WORD)e["scan"].num(),e["down"].boolean(),e["extended"].boolean());
            else {
                INPUT in{};in.type=INPUT_MOUSE;in.mi.dwExtraInfo=0x54535431;
                if(kind=="motion") {in.mi.dwFlags=MOUSEEVENTF_MOVE;in.mi.dx=(LONG)e["dx"].num();in.mi.dy=(LONG)e["dy"].num();}
                else if(kind=="button") in.mi.dwFlags=e["down"].boolean()?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP;
                else if(kind=="wheel") {in.mi.dwFlags=e["horizontal"].boolean()?MOUSEEVENTF_HWHEEL:MOUSEEVENTF_WHEEL;in.mi.mouseData=(DWORD)(LONG)e["delta"].num();}
                else continue;
                ok=SendInput(1,&in,sizeof in)==1;
            }
            if(!ok) commandError="SendInput was refused";
            Sleep((DWORD)e["wait_ms"].num(20));
        }
    } else if(op=="restart") {
        if(mesh) PostMessageW(mesh,RegisterWindowMessageW(L"LzcMeshQuit"),0,0);
        for(int i=0;i<200&&FindWindowW(L"LzcMeshMain",nullptr);i++) Sleep(50);
        if(FindWindowW(L"LzcMeshMain",nullptr)){commandError="Client did not quit";return;}
        wchar_t local[MAX_PATH];GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
        std::wstring exe=L"\""+std::wstring(local)+L"\\Programs\\LZC Mesh\\LZC Mesh.exe\" --background";
        STARTUPINFOW startup{sizeof startup};PROCESS_INFORMATION process{};
        if(!CreateProcessW(nullptr,exe.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)){commandError="Client restart failed";return;}
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
    } else if(op=="hide") {if(mesh) PostMessageW(mesh,WM_CLOSE,0,0);}
    else if(op=="show") {if(mesh){ShowWindow(mesh,SW_SHOW);SetForegroundWindow(mesh);}}
    else if(op=="scroll") {if(mesh) SendMessageW(mesh,WM_VSCROLL,(WPARAM)c["code"].num(),0);}
}
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);FillRect(dc,&r,(HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(35,35,35));
        const wchar_t *text=L"懒猫设备接力 · 自动测试窗口\n\n只接收测试输入，不操作现有应用和文档。\n测试结束后会自动关闭，并恢复原光标位置。";
        DrawTextW(dc,text,-1,&r,DT_CENTER|DT_VCENTER|DT_WORDBREAK);EndPaint(h,&ps);return 0;
    }
    case WM_MOUSEMOVE:
        moves++;
        if(positions.size()<2048)positions.push_back(JsonOut().n("x",GET_X_LPARAM(l)).n("y",GET_Y_LPARAM(l)).n("marker",(double)GetMessageExtraInfo()).str());
        return 0;
    case WM_LBUTTONDOWN:case WM_LBUTTONUP:mouseAction("button",1,m==WM_LBUTTONDOWN,l);return 0;
    case WM_RBUTTONDOWN:case WM_RBUTTONUP:mouseAction("button",2,m==WM_RBUTTONDOWN,l);return 0;
    case WM_MOUSEWHEEL:wheelY+=GET_WHEEL_DELTA_WPARAM(w);return 0;
    case WM_MOUSEHWHEEL:wheelX+=GET_WHEEL_DELTA_WPARAM(w);return 0;
    case WM_KEYDOWN:case WM_KEYUP:case WM_SYSKEYDOWN:case WM_SYSKEYUP:
        record(JsonOut().s("kind","key").n("vk",(double)w).b("down",m==WM_KEYDOWN||m==WM_SYSKEYDOWN).str());return 0;
    case WM_TIMER: {
        std::ifstream input(statusPath+".command");std::string text((std::istreambuf_iterator<char>(input)),{});input.close();
        Json value;
        if(!text.empty()&&Json::parse(text,value)){DeleteFileA((statusPath+".command").c_str());command(value);}
        if(GetTickCount()-started>600000){DestroyWindow(h);return 0;}
        snapshot();return 0;
    }
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:snapshot();PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HDESK desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    wchar_t name[64]{};DWORD needed=0;
    const bool unlocked=desktop && GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof name,&needed) && wcscmp(name,L"Default")==0;
    if(desktop)CloseDesktop(desktop);
    if(!unlocked)return 3; // Never unlock or inject into a secure desktop.
    char home[MAX_PATH];DWORD n=GetEnvironmentVariableA("USERPROFILE",home,MAX_PATH);if(!n||n>=MAX_PATH)return 2;
    statusPath=std::string(home)+"\\mesh-parity\\observer.json";
    GetCursorPos(&previousCursor);previousForeground=GetForegroundWindow();started=GetTickCount();
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=proc;wc.lpszClassName=L"LzcMeshTestObserver";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&wc);
    RECT r;SystemParametersInfoW(SPI_GETWORKAREA,0,&r,0);
    window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"LZC Mesh input test",WS_POPUP|WS_VISIBLE,r.left,r.top,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);
    SetForegroundWindow(window);SetFocus(window);SetTimer(window,1,100,nullptr);
    MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    SetCursorPos(previousCursor.x,previousCursor.y);if(IsWindow(previousForeground))SetForegroundWindow(previousForeground);
    return 0;
}
