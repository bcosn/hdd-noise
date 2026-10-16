// Optional native desktop integration test. No audio is started or played.
#define wWinMain HddNoiseOriginalEntry
#include "../App.cpp"
#undef wWinMain
#include <iostream>
#include <stdexcept>

void RequireProbe(bool pass,const char* reason){if(!pass)throw std::runtime_error(reason);}
void PumpProbe(){for(int i=0;i<10;++i){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(15);}}
int IconSide(HICON icon){ICONINFO info{};RequireProbe(GetIconInfo(icon,&info)!=FALSE,"GetIconInfo failed");BITMAP bm{};GetObjectW(info.hbmColor,sizeof(bm),&bm);if(info.hbmColor)DeleteObject(info.hbmColor);if(info.hbmMask)DeleteObject(info.hbmMask);return bm.bmWidth;}
bool IsLowIntegrity(){
    DWORD needed=0;HANDLE token{};OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token);
    GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&needed);std::vector<char> data(needed);
    if(GetTokenInformation(token,TokenIntegrityLevel,data.data(),needed,&needed)){
        auto label=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data());auto sid=label->Label.Sid;
        const DWORD rid=*GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1);
        CloseHandle(token);return rid<SECURITY_MANDATORY_MEDIUM_RID;
    }CloseHandle(token);return false;
}
int main(){

    int result=0;
    try{
        g_instance=GetModuleHandleW(nullptr);
        WNDCLASSW wc{};wc.hInstance=g_instance;wc.lpfnWndProc=WindowProc;wc.lpszClassName=L"HddNoiseTrayProbe";
        RequireProbe(RegisterClassW(&wc)!=0,"probe class registration");
        g_window=CreateWindowExW(0,wc.lpszClassName,L"hdd-noise tray validation",WS_OVERLAPPED,-32000,-32000,32,32,nullptr,nullptr,g_instance,nullptr);
        RequireProbe(g_window!=nullptr,"hidden probe window");
        KillTimer(g_window,1);
        RequireProbe(SendMessageW(C(Profile),CB_GETCOUNT,0,0)==4,"four profile choices");
        SelectProfile(2);RequireProbe(g_settings.rpm==5400,"laptop 5400 preset default");
        SelectProfile(3);RequireProbe(g_settings.rpm==7200,"laptop 7200 preset default");
        g_settings.pulseNoise=true;UpdateValues();RequireProbe(IsWindowEnabled(C(PulseRate)),"pulse rate control enabled");
        g_settings.pulseNoise=false;UpdateValues();RequireProbe(!IsWindowEnabled(C(PulseRate)),"pulse rate control disabled");
        g_dirty=false;
        // Never replace a running user's real notification icon.
        g_trayGuid.Data1^=GetCurrentProcessId();g_trayGuid.Data4[7]=0xee;
        for(double scale:{1.0,1.25,1.5,2.0}){
            g_scale=scale;RequireProbe(LoadAppIcons(),"embedded icons failed to load");
            RequireProbe(IconSide(g_smallIcon)==P(16),"small icon DPI size");
            RequireProbe(IconSide(g_icon)==P(32),"large icon DPI size");
        }
        g_snapshot.available=true;g_snapshot.state=hdd::SpindleState::Running;g_snapshot.rpm=7200;
        g_sample.available=true;g_sample.busy=.42;
        SetLastError(0);SetTray(true);PumpProbe();
        std::cout<<"Tray added="<<g_trayAdded<<" GUID="<<g_trayUseGuid<<" error="<<GetLastError()<<" shell="<<GetShellWindow()<<"\n";
        if(!g_trayAdded&&(IsLowIntegrity()||!GetShellWindow())){
            std::cout<<"Desktop controls/icons passed. Shell checks skipped: requires a normal interactive desktop token.\n";
            result=77;
        }else{
        RequireProbe(g_trayAdded,"Shell_NotifyIcon add failed");
        RequireProbe(g_trayVersion4,"version 4 notification callbacks");
        RequireProbe(g_trayTip.find(L"7200")!=std::wstring::npos&&g_trayTip.find(L"42%")!=std::wstring::npos,"status tooltip");
        NOTIFYICONIDENTIFIER identifier{};identifier.cbSize=sizeof(identifier);identifier.hWnd=g_window;identifier.uID=1;if(g_trayUseGuid)identifier.guidItem=g_trayGuid;
        RECT rect{};RequireProbe(SUCCEEDED(Shell_NotifyIconGetRect(&identifier,&rect)),"notification icon bounds");
        RequireProbe(rect.right>rect.left&&rect.bottom>rect.top,"nonempty tray icon bounds");
        WindowProc(g_window,WM_SIZE,SIZE_MINIMIZED,0);
        RequireProbe(!IsWindowVisible(g_window),"minimize hides to registered tray");
        WindowProc(g_window,kTray,0,MAKELPARAM(NIN_SELECT,1));
        RequireProbe(IsWindowVisible(g_window),"tray selection restores panel");
        ShowWindow(g_window,SW_HIDE);
        g_settings.master=0;SetTray(true);
        RequireProbe(g_trayAdded&&g_trayTip.find(L"\u9759\u97f3")!=std::wstring::npos,"tooltip modify");
        SetTray(false);PumpProbe();
        RequireProbe(FAILED(Shell_NotifyIconGetRect(&identifier,&rect)),"notification icon removal");
        g_taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        WindowProc(g_window,g_taskbarCreated,0,0);PumpProbe();
        RequireProbe(g_trayAdded&&g_trayVersion4,"TaskbarCreated recovery");
        std::cout<<"Tray integration passed: embedded icons at 4 DPI scales; actual shell add, v4, bounds, tooltip update, removal, TaskbarCreated recovery.\n";
        }
    }catch(const std::exception& e){std::cerr<<"Tray probe failed: "<<e.what()<<'\n';result=1;}
    SetTray(false);
    g_dirty=false;if(g_window)DestroyWindow(g_window);
    if(g_icon)DestroyIcon(g_icon);g_icon=nullptr;
    if(g_smallIcon)DestroyIcon(g_smallIcon);g_smallIcon=nullptr;
    return result;
}
