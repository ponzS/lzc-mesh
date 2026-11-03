// Read-only, 30-second mouse diagnostic. No focus, capture, injection, keyboard,
// device names, documents or clipboard. Pass the existing hook result unchanged.
#include <windows.h>
#include <fstream>
#include <string>
#include "../lzc-mesh-client/windows/src/json.h"
namespace {
HWND window;HHOOK hook;DWORD started;std::string path;
uint64_t moves=0,injected=0,rawRelative=0,rawAbsolute=0;
POINT proposed{},before{},after{};LONG rawX=0,rawY=0;
LRESULT CALLBACK observe(int code,WPARAM message,LPARAM value) {
    bool record=code==HC_ACTION && message==WM_MOUSEMOVE;
    auto *event=reinterpret_cast<MSLLHOOKSTRUCT*>(value);
    if(record) {
        if(event->flags&LLMHF_INJECTED) {++injected;record=false;}
        else {++moves;proposed=event->pt;GetCursorPos(&before);}
    }
    const auto result=CallNextHookEx(nullptr,code,message,value);
    if(record)GetCursorPos(&after);
    return result;
}
void snapshot() {
    POINT point{};RECT clip{};GetCursorPos(&point);GetClipCursor(&clip);
    CURSORINFO cursor{sizeof cursor};GetCursorInfo(&cursor);
    const auto text=JsonOut().n("moves",double(moves)).n("injected_moves",double(injected))
        .n("proposed_x",proposed.x).n("proposed_y",proposed.y).n("before_x",before.x).n("before_y",before.y).n("after_x",after.x).n("after_y",after.y)
        .n("cursor_x",point.x).n("cursor_y",point.y).b("cursor_visible",(cursor.flags&CURSOR_SHOWING)!=0)
        .n("clip_left",clip.left).n("clip_top",clip.top).n("clip_right",clip.right).n("clip_bottom",clip.bottom)
        .n("raw_relative",double(rawRelative)).n("raw_absolute",double(rawAbsolute)).n("raw_x",rawX).n("raw_y",rawY)
        .n("screen_x",GetSystemMetrics(SM_XVIRTUALSCREEN)).n("screen_y",GetSystemMetrics(SM_YVIRTUALSCREEN)).n("screen_w",GetSystemMetrics(SM_CXVIRTUALSCREEN)).n("screen_h",GetSystemMetrics(SM_CYVIRTUALSCREEN))
        .n("dpi",GetDpiForSystem()).b("hook",hook!=nullptr).str();
    std::ofstream out(path,std::ios::app);out<<text<<'\n';
}
LRESULT CALLBACK proc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_INPUT) {
        RAWINPUT input{};UINT size=sizeof input;
        if(GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER))!=UINT(-1)&&input.header.dwType==RIM_TYPEMOUSE) {
            if(input.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)++rawAbsolute;else ++rawRelative;
            rawX=input.data.mouse.lLastX;rawY=input.data.mouse.lLastY;
        }
    }
    if(message==WM_TIMER) {snapshot();if(GetTickCount()-started>=30000)PostQuitMessage(0);return 0;}
    return DefWindowProcW(h,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HDESK desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);wchar_t name[64]{};DWORD needed=0;
    const bool ready=desktop&&GetUserObjectInformationW(desktop,UOI_NAME,name,sizeof name,&needed)&&wcscmp(name,L"Default")==0;
    if(desktop)CloseDesktop(desktop);if(!ready)return 3;
    char home[MAX_PATH];if(!GetEnvironmentVariableA("USERPROFILE",home,MAX_PATH))return 2;
    path=std::string(home)+"\\mesh-parity\\pointer-probe.jsonl";
    {std::ofstream clear(path);}
    WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=instance;wc.lpszClassName=L"LzcMeshReadOnlyPointerProbe";RegisterClassW(&wc);
    window=CreateWindowExW(0,wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,instance,nullptr);
    RAWINPUTDEVICE device{1,2,RIDEV_INPUTSINK,window};RegisterRawInputDevices(&device,1,sizeof device);
    hook=SetWindowsHookExW(WH_MOUSE_LL,observe,instance,0);started=GetTickCount();SetTimer(window,1,250,nullptr);
    MSG message;while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    if(hook)UnhookWindowsHookEx(hook);DestroyWindow(window);
}
