// hdd-noise — native Win32 UI. Every sound is synthesized in Synth.cpp.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include "AudioEngine.h"
#include "DiskMonitor.h"
#include "Synth.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
constexpr wchar_t kClass[] = L"hdd-noiseWindow";
constexpr wchar_t kTitle[] = L"机械硬盘 · hdd-noise";
constexpr UINT kTray = WM_APP + 1;
constexpr int kWidth = 584, kHeight = 626;
enum : int { Disk = 100, Rpm, Interval, Random, Profile, PulseToggle, PulseRate,
    Master = 110, Motor, Seek, Impact, Pulse, Clarity,
    Power = 200, TestSeek, TestImpact, Hide, Quit,
    StateLabel = 300, ActivityLabel, IoLabel, ActivityBar, NoticeLabel };
struct Control { HWND hwnd; int id, x, y, w, h; };
HWND g_window{};
HINSTANCE g_instance{};
UINT g_taskbarCreated{};
double g_scale = 1.0;
HFONT g_font{};
HICON g_icon{};
HICON g_smallIcon{};
// Stable identity preserves the user's notification-area visibility preference.
GUID g_trayGuid{0x5c240dd7,0x8226,0x4422,{0x96,0x5e,0x42,0x10,0x38,0x7b,0x68,0xa1}};
bool g_trayVersion4=false, g_trayUseGuid=true;
std::wstring g_trayTip,g_trayNotice;
DWORD g_trayError=ERROR_SUCCESS;
ULONGLONG g_nextTrayRefresh{},g_trayMissingSince{};
double g_unmuteVolume=.35;
std::vector<Control> g_controls;
std::vector<hdd::DiskInfo> g_disks;
hdd::SynthSettings g_settings;
hdd::AudioEngine g_audio;
hdd::DiskMonitor g_monitor;
hdd::DiskSample g_sample;
hdd::EngineSnapshot g_snapshot;
std::wstring g_ini, g_diskId = L"_Total", g_notice;
bool g_running = true, g_exiting = false, g_trayAdded = false, g_dirty = false;
ULONGLONG g_exitStarted{}, g_saveAt{};
int P(double n) { return static_cast<int>(std::lround(n * g_scale)); }
HWND C(int id) { for (const auto& c : g_controls) if (c.id == id) return c.hwnd; return nullptr; }

std::wstring Fixed(double value, int decimals = 0) {
    wchar_t out[80]; swprintf_s(out, L"%.*f", decimals, value); return out;
}
// Use the operating system's dialog font and unmodified standard controls.
void MakeFonts() {
    HFONT oldFont = g_font;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    bool found = false;
    if (auto user = GetModuleHandleW(L"user32.dll")) {
        auto getMetrics = reinterpret_cast<BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT)>(
            GetProcAddress(user, "SystemParametersInfoForDpi"));
        if (getMetrics) found = getMetrics(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
            &metrics, 0, static_cast<UINT>(P(96))) != FALSE;
    }
    if (!found) {
        found = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;
        if (found) {
            HDC screen = GetDC(nullptr);
            int sourceDpi = GetDeviceCaps(screen, LOGPIXELSY);
            ReleaseDC(nullptr, screen);
            metrics.lfMessageFont.lfHeight = MulDiv(metrics.lfMessageFont.lfHeight, P(96), sourceDpi);
        }
    }
    g_font = found ? CreateFontIndirectW(&metrics.lfMessageFont) : nullptr;
    for (const auto& c : g_controls)
        SendMessageW(c.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    if (oldFont) DeleteObject(oldFont);
}
HWND Add(const wchar_t* cls, const wchar_t* text, int id, int x, int y, int w, int h,
         DWORD style, bool tabStop = true) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (tabStop ? WS_TABSTOP : 0) | style,
        P(x), P(y), P(w), P(h), g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    g_controls.push_back({hwnd, id, x, y, w, h});
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font ? g_font : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return hwnd;
}
void AddLabel(const wchar_t* text, int x, int y, int w, int h = 22, int id = -1, DWORD style = SS_LEFT) {
    Add(WC_STATICW, text, id, x, y, w, h, style, false);
}
void AddGroup(const wchar_t* text, int x, int y, int w, int h) {
    Add(WC_BUTTONW, text, -1, x, y, w, h, BS_GROUPBOX, false);
}
void SetText(int id, const std::wstring& text) {
    wchar_t current[2048]{};
    GetWindowTextW(C(id), current, 2048);
    if (text != current) SetWindowTextW(C(id), text.c_str());
}
void AddCombo(int id, int x, int y, int w) {
    Add(WC_COMBOBOXW, L"", id, x, y, w, 180, CBS_DROPDOWNLIST | WS_VSCROLL);
}
void AddSlider(int id,int x,int y,int w,double value) {
    HWND c=Add(TRACKBAR_CLASSW,L"",id,x,y,w,30,TBS_HORZ|TBS_NOTICKS);
    SendMessageW(c,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
    SendMessageW(c,TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(value*100)));
    SendMessageW(c,TBM_SETPAGESIZE,0,5);
}
void AddButton(int id,const wchar_t* label,int x,int y,int w,int h=30) {
    Add(WC_BUTTONW,label,id,x,y,w,h,BS_PUSHBUTTON);
}
double ReadNumber(const wchar_t* key, double fallback, double lo, double hi) {
    wchar_t s[128]; GetPrivateProfileStringW(L"Sound",key,L"",s,128,g_ini.c_str());
    wchar_t* end=nullptr; double v=wcstod(s,&end);
    if (!s[0]||end==s||*end||!std::isfinite(v)) return fallback;
    return std::clamp(v,lo,hi);
}
void LoadSettings() {
    wchar_t exe[32768]; DWORD n=GetModuleFileNameW(nullptr,exe,32768);
    std::filesystem::path dir=std::filesystem::path(std::wstring(exe,n)).parent_path();
    g_ini=(dir/L"hdd-noise.ini").wstring();
    g_settings.master=ReadNumber(L"Master",.35,0,1);
    g_settings.motor=ReadNumber(L"Motor",.55,0,1);
    g_settings.seek=ReadNumber(L"Seek",.65,0,1);
    g_settings.impact=ReadNumber(L"Impact",.5,0,1);
    g_settings.pulse=ReadNumber(L"Pulse",.30,0,1);
    g_settings.clarity=ReadNumber(L"Clarity",.65,0,1);
    g_settings.pulseRate=ReadNumber(L"PulseRate",8,2,40);
    g_settings.pulseNoise=GetPrivateProfileIntW(L"Sound",L"PulseNoise",0,g_ini.c_str())!=0;
    g_settings.rpm=ReadNumber(L"RPM",7200,3600,15000);
    g_settings.impactInterval=ReadNumber(L"ImpactInterval",18,3,120);
    g_settings.randomImpacts=GetPrivateProfileIntW(L"Sound",L"RandomImpacts",1,g_ini.c_str())!=0;
    const int profile=GetPrivateProfileIntW(L"Sound",L"Profile",0,g_ini.c_str());
    g_settings.profile=static_cast<hdd::DriveProfile>(profile>=0&&profile<4?profile:0);
    wchar_t disk[256]; GetPrivateProfileStringW(L"Monitor",L"Disk",L"_Total",disk,256,g_ini.c_str());
    g_diskId=disk;
}
bool SaveSettings() {
    bool ok=true;
    auto write=[&](const wchar_t* key,double v) { auto s=Fixed(v,4);
        if(!WritePrivateProfileStringW(L"Sound",key,s.c_str(),g_ini.c_str())) ok=false; };
    write(L"Master",g_settings.master); write(L"Motor",g_settings.motor);
    write(L"Seek",g_settings.seek); write(L"Impact",g_settings.impact);
    write(L"Pulse",g_settings.pulse);write(L"PulseRate",g_settings.pulseRate);write(L"Clarity",g_settings.clarity);
    if(!WritePrivateProfileStringW(L"Sound",L"PulseNoise",g_settings.pulseNoise?L"1":L"0",g_ini.c_str()))ok=false;
    write(L"RPM",g_settings.rpm); write(L"ImpactInterval",g_settings.impactInterval);
    write(L"Profile",static_cast<int>(g_settings.profile));
    if(!WritePrivateProfileStringW(L"Sound",L"RandomImpacts",g_settings.randomImpacts?L"1":L"0",g_ini.c_str()))ok=false;
    if(!WritePrivateProfileStringW(L"Monitor",L"Disk",g_diskId.c_str(),g_ini.c_str()))ok=false;
    g_dirty=false;
    if(!ok)g_notice=L"设置未能保存，请把程序放到可写入的文件夹。";
    return ok;
}
void UpdateValues();
void UpdateStatus();
void Changed() {
    g_audio.setSettings(g_settings);
    g_dirty=true; g_saveAt=GetTickCount64()+600;
    UpdateValues();
}
bool LoadAppIcons() {
    HICON large=static_cast<HICON>(LoadImageW(g_instance,MAKEINTRESOURCEW(IDI_APP),IMAGE_ICON,P(32),P(32),0));
    HICON small=static_cast<HICON>(LoadImageW(g_instance,MAKEINTRESOURCEW(IDI_APP),IMAGE_ICON,P(16),P(16),0));
    if(!large||!small){if(large)DestroyIcon(large);if(small)DestroyIcon(small);return false;}
    HICON oldLarge=g_icon,oldSmall=g_smallIcon;
    g_icon=large;g_smallIcon=small;
    if(g_window){
        SetClassLongPtrW(g_window,GCLP_HICON,reinterpret_cast<LONG_PTR>(g_icon));
        SetClassLongPtrW(g_window,GCLP_HICONSM,reinterpret_cast<LONG_PTR>(g_smallIcon));
        SendMessageW(g_window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(g_icon));
        SendMessageW(g_window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(g_smallIcon));
    }
    if(oldLarge)DestroyIcon(oldLarge);if(oldSmall)DestroyIcon(oldSmall);
    return true;
}
std::wstring TrayTip() {
    std::wstring state;
    if(g_exiting)state=L"正在停转并退出";
    else if(!g_snapshot.available)state=L"音频设备未就绪";
    else if(g_snapshot.state==hdd::SpindleState::Running)state=L"运行中";
    else if(g_snapshot.state==hdd::SpindleState::Starting)state=L"正在加速";
    else if(g_snapshot.state==hdd::SpindleState::Stopping)state=L"正在减速";
    else state=L"已停止";
    if(g_settings.master==0)state+=L" · 静音";
    return L"机械硬盘 · "+state+L"\n"+Fixed(g_snapshot.rpm)+L" RPM  磁盘活动 "+
        (g_sample.available?Fixed(g_sample.busy*100)+L"%":L"—")+L"\n单击打开，右键菜单";
}
DWORD ProcessIntegrityRid() {
    HANDLE token{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return MAXDWORD;
    DWORD needed=0;
    GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&needed);
    std::vector<BYTE> buffer(needed);
    const bool read=needed&&GetTokenInformation(token,TokenIntegrityLevel,buffer.data(),needed,&needed);
    CloseHandle(token);
    if(!read)return MAXDWORD;
    const auto label=reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    PSID sid=label->Label.Sid;
    if(!IsValidSid(sid)||!(*GetSidSubAuthorityCount(sid)))return MAXDWORD;
    return *GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1);
}
std::wstring TrayFailureMessage() {
    if(ProcessIntegrityRid()<SECURITY_MANDATORY_MEDIUM_RID)
        return L"程序处于 Windows 受限运行状态，无法使用托盘。请将发行包解压到普通文件夹后运行，或参见 README 的托盘排查说明。";
    if(!FindWindowW(L"Shell_TrayWnd",nullptr))
        return L"Windows 任务栏尚未就绪，正在重试；窗口会保持可用。";
    return L"托盘注册失败"+(g_trayError?L"（系统代码 "+std::to_wstring(g_trayError)+L"）":L"。")+
        L"窗口会保持可用并重试；可用 --diagnose-tray 导出诊断。";
}
NOTIFYICONDATAW TrayData() {
    NOTIFYICONDATAW n{};n.cbSize=sizeof(n);n.hWnd=g_window;n.uID=1;
    if(g_trayUseGuid){n.uFlags=NIF_GUID;n.guidItem=g_trayGuid;}return n;
}
void SetTray(bool add) {
    auto n=TrayData();
    if(!add){if(g_trayAdded)Shell_NotifyIconW(NIM_DELETE,&n);g_trayAdded=false;g_trayVersion4=false;return;}
    n.uFlags|=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP;
    n.uCallbackMessage=kTray;n.hIcon=g_smallIcon;
    const auto tip=TrayTip();wcsncpy_s(n.szTip,tip.c_str(),_TRUNCATE);
    if(g_trayAdded&&Shell_NotifyIconW(NIM_MODIFY,&n)){g_trayTip=tip;g_trayError=0;g_trayNotice.clear();return;}
    SetLastError(ERROR_SUCCESS);
    g_trayAdded=Shell_NotifyIconW(NIM_ADD,&n)!=FALSE;
    g_trayError=g_trayAdded?ERROR_SUCCESS:GetLastError();
    // Windows binds GUID icons to an executable path. Portable unsigned builds
    // can move; fall back to the window/uID identity when GUID registration fails.
    if(!g_trayAdded&&g_trayUseGuid){
        g_trayUseGuid=false;n.uFlags&=~NIF_GUID;
        SetLastError(ERROR_SUCCESS);
        g_trayAdded=Shell_NotifyIconW(NIM_ADD,&n)!=FALSE;
        g_trayError=g_trayAdded?ERROR_SUCCESS:GetLastError();
    }
    g_trayVersion4=false;
    if(g_trayAdded){
        n.uVersion=NOTIFYICON_VERSION_4;
        g_trayVersion4=Shell_NotifyIconW(NIM_SETVERSION,&n)!=FALSE;
        g_trayTip=tip;g_trayMissingSince=0;g_trayNotice.clear();
    }else{
        if(!g_trayMissingSince)g_trayMissingSince=GetTickCount64();
        g_trayNotice=TrayFailureMessage();
    }
}
void RefreshTray() {
    const auto now=GetTickCount64();if(now<g_nextTrayRefresh)return;
    g_nextTrayRefresh=now+(g_trayAdded?500:2000);
    if(!g_trayAdded||g_trayTip!=TrayTip())SetTray(true);
    // A missing Explorer must not leave the only control window inaccessible.
    if(!g_trayAdded&&!g_exiting&&g_trayMissingSince&&now-g_trayMissingSince>5000&&!IsWindowVisible(g_window))
        ShowWindow(g_window,SW_SHOWNA);
}
void ShowPanel() { ShowWindow(g_window,SW_RESTORE); SetForegroundWindow(g_window); }
void SetRunning(bool running) {
    if(g_exiting)return;
    if(running&&!g_audio.snapshot().available)g_audio.start();
    g_running=running;g_audio.setRunning(running);
    SetWindowTextW(C(Power),running?L"停止转动":L"启动硬盘");
}
void ToggleMute() {
    if(g_exiting)return;
    if(g_settings.master>0){g_unmuteVolume=g_settings.master;g_settings.master=0;}
    else g_settings.master=g_unmuteVolume;
    SendMessageW(C(Master),TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(g_settings.master*100)));
    Changed();
}
void TogglePulse() {
    if(g_exiting)return;
    g_settings.pulseNoise=!g_settings.pulseNoise;
    SendMessageW(C(PulseToggle),BM_SETCHECK,g_settings.pulseNoise?BST_CHECKED:BST_UNCHECKED,0);
    Changed();
}
void BeginExit() {
    if(g_exiting)return;
    g_exiting=true;g_running=false;g_exitStarted=GetTickCount64();
    g_audio.setRunning(false); SetWindowTextW(C(Power),L"正在停转…");
    for(int id:{Power,TestSeek,TestImpact,Rpm,Interval,Random,Profile,PulseToggle,Pulse,PulseRate,Clarity,Master,Motor,Seek,Impact})EnableWindow(C(id),FALSE);
    UpdateStatus();
}
void UpdateDisks() {
    auto disks=g_monitor.disks();
    if(disks.empty())disks.push_back({L"_Total",L"所有物理磁盘"});
    if(std::none_of(disks.begin(),disks.end(),[](const hdd::DiskInfo& d){return d.id==g_diskId;}))
        disks.push_back({g_diskId,g_diskId+L"（等待连接）"});
    bool changed=disks.size()!=g_disks.size();
    if(!changed) for(size_t i=0;i<disks.size();++i)if(disks[i].id!=g_disks[i].id||disks[i].label!=g_disks[i].label){changed=true;break;}
    if(!changed)return;
    g_disks=std::move(disks); SendMessageW(C(Disk),CB_RESETCONTENT,0,0);
    int selected=0;
    for(size_t i=0;i<g_disks.size();++i) {
        SendMessageW(C(Disk),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(g_disks[i].label.c_str()));
        if(g_disks[i].id==g_diskId)selected=static_cast<int>(i);
    }
    SendMessageW(C(Disk),CB_SETCURSEL,selected,0);
}
void PopulatePresets(int id,const std::vector<double>& values,double current,const wchar_t* suffix) {
    std::vector<double> list=values;
    if(std::none_of(list.begin(),list.end(),[&](double v){return std::abs(v-current)<.01;}))list.push_back(current);
    std::sort(list.begin(),list.end()); int selected=0;
    for(size_t i=0;i<list.size();++i){
        auto label=Fixed(list[i])+suffix;
        auto index=SendMessageW(C(id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(C(id),CB_SETITEMDATA,index,static_cast<LPARAM>(std::lround(list[i])));
        if(std::abs(list[i]-current)<.01)selected=static_cast<int>(i);
    }
    SendMessageW(C(id),CB_SETCURSEL,selected,0);
}
void UpdateValues() {
    const std::pair<int, double> gains[] = {
        {Master,g_settings.master}, {Clarity,g_settings.clarity}, {Motor,g_settings.motor}, {Seek,g_settings.seek},
        {Impact,g_settings.impact}, {Pulse,g_settings.pulse}
    };
    for (const auto& gain : gains) SetText(1000+gain.first, Fixed(gain.second*100)+L"%");
    EnableWindow(C(Pulse), g_settings.pulseNoise && !g_exiting);
    EnableWindow(C(PulseRate),g_settings.pulseNoise&&!g_exiting);
}
void UpdateStatus() {
    std::wstring state;
    if (g_exiting) state=L"正在停转，随后退出";
    else if (!g_snapshot.available) state=L"音频设备未就绪";
    else if (g_snapshot.state==hdd::SpindleState::Running) state=L"运行中";
    else if (g_snapshot.state==hdd::SpindleState::Starting) state=L"正在加速";
    else if (g_snapshot.state==hdd::SpindleState::Stopping) state=L"正在减速";
    else state=L"已停止";
    SetText(StateLabel, L"状态："+state+L"（"+Fixed(g_snapshot.rpm)+L" RPM）");
    SetText(ActivityLabel, g_sample.available ? L"磁盘活动："+Fixed(g_sample.busy*100)+L"%" : L"磁盘活动：等待数据");
    const int activity=g_sample.available ? static_cast<int>(std::lround(g_sample.busy*100)) : 0;
    if (SendMessageW(C(ActivityBar),PBM_GETPOS,0,0)!=activity)
        SendMessageW(C(ActivityBar),PBM_SETPOS,activity,0);
    SetText(IoLabel, g_sample.available ? L"传输："+Fixed(g_sample.transfersPerSecond)+L" 次/秒    吞吐："+
        Fixed(g_sample.bytesPerSecond/1048576.0,1)+L" MiB/s" : L"传输：—    吞吐：—");
    std::wstring notice;
    if (!g_notice.empty()) notice=g_notice;
    else if (!g_trayNotice.empty()) notice=g_trayNotice;
    else if (!g_snapshot.available && !g_snapshot.error.empty()) notice=g_snapshot.error;
    else if (!g_sample.available) notice=g_sample.status;
    else notice=L"脉冲声为间歇短脉冲；设置自动保存。";
    SetText(NoticeLabel,notice);
}
void CreateControls() {
    AddGroup(L"运行状态",12,8,560,96);
    AddLabel(L"状态：正在启动",24,29,394,22,StateLabel);
    AddButton(Power,L"停止转动",432,26,124);
    AddLabel(L"磁盘活动：等待数据",24,59,170,20,ActivityLabel);
    Add(PROGRESS_CLASSW,L"",ActivityBar,196,60,360,17,0,false);
    SendMessageW(C(ActivityBar),PBM_SETRANGE32,0,100);
    AddLabel(L"传输：—    吞吐：—",24,81,532,20,IoLabel);

    AddGroup(L"硬盘设置",12,116,560,132);
    AddLabel(L"监测磁盘：",24,140,80);
    AddCombo(Disk,108,137,448);
    AddLabel(L"音色参考：",24,176,80);
    AddCombo(Profile,108,173,220);
    SendMessageW(C(Profile),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"WD1600JD 参考"));
    SendMessageW(C(Profile),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"日立 T7K500 参考"));
    SendMessageW(C(Profile),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"笔记本 2.5寸 · 5400"));
    SendMessageW(C(Profile),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"笔记本 2.5寸 · 7200"));
    SendMessageW(C(Profile),CB_SETCURSEL,static_cast<int>(g_settings.profile),0);
    AddLabel(L"转速：",352,176,42);
    AddCombo(Rpm,396,173,160);
    PopulatePresets(Rpm,{3600,5400,7200,10000,15000},g_settings.rpm,L" RPM");
    Add(WC_BUTTONW,L"随机撞击",Random,24,210,124,25,BS_AUTOCHECKBOX);
    SendMessageW(C(Random),BM_SETCHECK,g_settings.randomImpacts?BST_CHECKED:BST_UNCHECKED,0);
    AddLabel(L"平均间隔：",166,213,72);
    AddCombo(Interval,240,209,140);
    PopulatePresets(Interval,{5,10,18,30,60,120},g_settings.impactInterval,L" 秒");

    AddLabel(L"脉冲频率：",392,213,78);
    AddCombo(PulseRate,474,209,82);
    PopulatePresets(PulseRate,{2,4,8,12,20,30,40},g_settings.pulseRate,L" Hz");

    AddGroup(L"声音",12,260,560,260);
    struct GainRow { int id; const wchar_t* label; double value; };
    const GainRow rows[] = {
        {Master,L"总音量",g_settings.master}, {Clarity,L"通透度",g_settings.clarity},
        {Motor,L"电机 / 旋转声",g_settings.motor},
        {Seek,L"寻道声",g_settings.seek}, {Impact,L"撞击声",g_settings.impact},
        {Pulse,L"脉冲声",g_settings.pulse}
    };
    int y=286;
    for (const auto& row : rows) {
        if (row.id==Pulse) {
            Add(WC_BUTTONW,row.label,PulseToggle,24,y,130,25,BS_AUTOCHECKBOX);
            SendMessageW(C(PulseToggle),BM_SETCHECK,g_settings.pulseNoise?BST_CHECKED:BST_UNCHECKED,0);
        } else AddLabel(row.label,24,y+3,126);
        AddSlider(row.id,160,y-1,340,row.value);
        AddLabel(L"",508,y+3,48,22,1000+row.id,SS_RIGHT);
        y+=36;
    }
    AddButton(TestSeek,L"试听寻道",12,536,132);
    AddButton(TestImpact,L"试听撞击",150,536,132);
    AddButton(Hide,L"收进托盘",288,536,132);
    AddButton(Quit,L"停转并退出",426,536,146);
    AddLabel(L"正在读取磁盘活动…",12,580,560,36,NoticeLabel);
    UpdateValues();
    UpdateDisks();
}
void SelectProfile(int index) {
    if(g_exiting||index<0||index>3)return;
    g_settings.profile=static_cast<hdd::DriveProfile>(index);
    g_settings.rpm=index==2?5400.0:7200.0;
    SendMessageW(C(Profile),CB_SETCURSEL,index,0);
    for(int i=0;i<SendMessageW(C(Rpm),CB_GETCOUNT,0,0);++i)
        if(SendMessageW(C(Rpm),CB_GETITEMDATA,i,0)==static_cast<LPARAM>(g_settings.rpm))
            SendMessageW(C(Rpm),CB_SETCURSEL,i,0);
    Changed();
}
void TrayMenu() {
    HMENU m=CreatePopupMenu();
    AppendMenuW(m,MF_STRING,1,L"打开控制面板");SetMenuDefaultItem(m,1,FALSE);
    const UINT usable=MF_STRING|(g_exiting?MF_GRAYED:0);
    AppendMenuW(m,usable,2,g_running?L"停止转动":L"启动硬盘");
    AppendMenuW(m,usable|(g_settings.master==0?MF_CHECKED:0),4,L"静音");
    AppendMenuW(m,usable|(g_settings.pulseNoise?MF_CHECKED:0),5,L"脉冲声");
    HMENU profiles=CreatePopupMenu();
    const wchar_t* names[]={L"WD1600JD 参考",L"日立 T7K500 参考",L"笔记本 2.5寸 · 5400",L"笔记本 2.5寸 · 7200"};
    for(int i=0;i<4;++i)AppendMenuW(profiles,usable|(i==static_cast<int>(g_settings.profile)?MF_CHECKED:0),20+i,names[i]);
    AppendMenuW(m,MF_POPUP|usable,reinterpret_cast<UINT_PTR>(profiles),L"硬盘预设");
    const bool canTest=!g_exiting&&g_snapshot.available&&g_snapshot.state==hdd::SpindleState::Running;
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    AppendMenuW(m,MF_STRING|(canTest?0:MF_GRAYED),6,L"试听寻道");
    AppendMenuW(m,MF_STRING|(canTest?0:MF_GRAYED),7,L"试听撞击");
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);AppendMenuW(m,MF_STRING|(g_exiting?MF_GRAYED:0),3,L"停转并退出");
    POINT p;GetCursorPos(&p);SetForegroundWindow(g_window);
    int selected=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,g_window,nullptr);
    DestroyMenu(m);PostMessageW(g_window,WM_NULL,0,0);
    if(selected==1)ShowPanel();if(selected==2)SetRunning(!g_running);if(selected==3)BeginExit();
    if(selected==4)ToggleMute();if(selected==5)TogglePulse();
    if(selected>=20&&selected<=23)SelectProfile(selected-20);
    if(selected==6)g_audio.triggerSeek();if(selected==7)g_audio.triggerImpact();
    if(selected!=1&&selected!=3){auto n=TrayData();Shell_NotifyIconW(NIM_SETFOCUS,&n);}
}
LRESULT CALLBACK WindowProc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==g_taskbarCreated&&g_taskbarCreated){g_trayAdded=false;g_nextTrayRefresh=0;SetTray(true);return 0;}
    switch(msg){
    case WM_CREATE:g_window=window;CreateControls();SetTimer(window,1,100,nullptr);return 0;
    case WM_HSCROLL:{int id=GetDlgCtrlID(reinterpret_cast<HWND>(lp));double value=SendMessageW(reinterpret_cast<HWND>(lp),TBM_GETPOS,0,0)/100.0;
        if(id==Master){g_settings.master=value;if(value>0)g_unmuteVolume=value;}if(id==Clarity)g_settings.clarity=value;if(id==Motor)g_settings.motor=value;if(id==Seek)g_settings.seek=value;if(id==Impact)g_settings.impact=value;if(id==Pulse)g_settings.pulse=value;Changed();return 0;}
    case WM_COMMAND:{int id=LOWORD(wp),code=HIWORD(wp);
        if(code==CBN_SELCHANGE){int index=static_cast<int>(SendMessageW(C(id),CB_GETCURSEL,0,0));if(index<0)return 0;
            if(id==Disk&&index<static_cast<int>(g_disks.size())){g_diskId=g_disks[index].id;g_monitor.select(g_diskId);}
            if(id==Rpm)g_settings.rpm=static_cast<double>(SendMessageW(C(id),CB_GETITEMDATA,index,0));
            if(id==Profile)SelectProfile(index);
            if(id==PulseRate)g_settings.pulseRate=static_cast<double>(SendMessageW(C(id),CB_GETITEMDATA,index,0));
            if(id==Interval)g_settings.impactInterval=static_cast<double>(SendMessageW(C(id),CB_GETITEMDATA,index,0));Changed();return 0;}
        if(code==BN_CLICKED){
            if(id==Power)SetRunning(!g_running);
            if(id==TestSeek)g_audio.triggerSeek();if(id==TestImpact)g_audio.triggerImpact();
            if(id==Hide){SetTray(true);if(g_trayAdded)ShowWindow(window,SW_HIDE);UpdateStatus();}
            if(id==Quit)BeginExit();
            if(id==Random){g_settings.randomImpacts=SendMessageW(C(Random),BM_GETCHECK,0,0)==BST_CHECKED;Changed();}
            if(id==PulseToggle){g_settings.pulseNoise=SendMessageW(C(PulseToggle),BM_GETCHECK,0,0)==BST_CHECKED;Changed();}
        }return 0;}
    case WM_TIMER:
        g_sample=g_monitor.sample();g_snapshot=g_audio.snapshot();
        g_audio.setActivity(g_sample.available?g_sample.busy:0,g_sample.available?g_sample.transfersPerSecond:0);
        UpdateDisks();if(g_dirty&&GetTickCount64()>=g_saveAt)SaveSettings();
        if(g_exiting&&((g_snapshot.state==hdd::SpindleState::Stopped&&GetTickCount64()-g_exitStarted>150)||!g_snapshot.available||GetTickCount64()-g_exitStarted>6500)){DestroyWindow(window);return 0;}
        EnableWindow(C(TestSeek),!g_exiting&&g_snapshot.available&&g_snapshot.state==hdd::SpindleState::Running);
        EnableWindow(C(TestImpact),!g_exiting&&g_snapshot.available&&g_snapshot.state==hdd::SpindleState::Running);
        RefreshTray();UpdateStatus();return 0;
    case WM_SIZE:if(wp==SIZE_MINIMIZED&&g_trayAdded)ShowWindow(window,SW_HIDE);return 0;
    case WM_DPICHANGED:{g_scale=HIWORD(wp)/96.0;MakeFonts();
        RECT* r=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        for(const auto& c:g_controls)SetWindowPos(c.hwnd,nullptr,P(c.x),P(c.y),P(c.w),P(c.h),SWP_NOZORDER|SWP_NOACTIVATE);
        if(LoadAppIcons())SetTray(true);
        return 0;}
    case kTray:{
        const UINT event=g_trayVersion4?LOWORD(lp):static_cast<UINT>(lp);
        if(event==NIN_SELECT||event==NIN_KEYSELECT||event==WM_LBUTTONDBLCLK||(!g_trayVersion4&&event==WM_LBUTTONUP))ShowPanel();
        else if((g_trayVersion4&&event==WM_CONTEXTMENU)||(!g_trayVersion4&&event==WM_RBUTTONUP))TrayMenu();
        return 0;}
    case WM_CLOSE:BeginExit();return 0;
    case WM_QUERYENDSESSION:g_audio.setRunning(false);if(g_dirty)SaveSettings();return TRUE;
    case WM_ENDSESSION:if(wp)DestroyWindow(window);else g_audio.setRunning(g_running&&!g_exiting);return 0;
    case WM_DESTROY:KillTimer(window,1);if(g_dirty)SaveSettings();SetTray(false);PostQuitMessage(0);return 0;
    }return DefWindowProcW(window,msg,wp,lp);
}

// Offline render uses exactly the same synth as real-time playback. No audio device needed.
int RenderDemo(const std::filesystem::path& path,const std::wstring& mode=L"wd") {
    constexpr uint32_t rate=48000,seconds=18,frames=rate*seconds,dataSize=frames*4;
    std::error_code ec;if(path.has_parent_path())std::filesystem::create_directories(path.parent_path(),ec);
    std::ofstream f(path,std::ios::binary);if(!f)return 2;
    auto u16=[&](uint16_t v){char b[2]={static_cast<char>(v),static_cast<char>(v>>8)};f.write(b,2);};
    auto u32=[&](uint32_t v){u16(static_cast<uint16_t>(v));u16(static_cast<uint16_t>(v>>16));};
    f.write("RIFF",4);u32(36+dataSize);f.write("WAVEfmt ",8);u32(16);u16(1);u16(2);u32(rate);u32(rate*4);u16(4);u16(16);f.write("data",4);u32(dataSize);
    hdd::Synth s(rate);hdd::SynthSettings settings;settings.randomImpacts=false;
    if(mode==L"hitachi")settings.profile=hdd::DriveProfile::Hitachi;
    if(mode==L"laptop5400"){settings.profile=hdd::DriveProfile::Laptop5400;settings.rpm=5400;}
    if(mode==L"laptop7200")settings.profile=hdd::DriveProfile::Laptop7200;
    const bool pulseOnly=mode==L"pulse",impactOnly=mode==L"impact";
    if(pulseOnly){settings.pulseNoise=true;settings.pulse=.65;settings.master=.5;settings.motor=settings.seek=settings.impact=0;}
    if(impactOnly){settings.master=.5;settings.impact=.65;settings.motor=settings.seek=0;}
    s.setSettings(settings);s.setRunning(true);
    for(uint32_t i=0;i<frames;++i){
        if(!pulseOnly&&!impactOnly){
            if(i==rate*6)s.setActivity(.9,160);
            if(i==rate*11){s.setActivity(.02,0);s.triggerImpact();}
            if(i==rate*13)s.triggerSeek();
        }
        if(impactOnly&&(i==rate*5||i==rate*8||i==rate*11||i==rate*13))s.triggerImpact();
        if(i==rate*15)s.setRunning(false);
        auto frame=s.next();for(float v:{frame.left,frame.right}){
            int16_t pcm=static_cast<int16_t>(std::lround(std::clamp(v,-1.0f,1.0f)*32767));u16(static_cast<uint16_t>(pcm));}
    }return f?0:3;
}
// Test the actual executable's launch identity and shell access without audio.
int TrayDiagnostic(const std::filesystem::path& path) {
    g_instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=g_instance;wc.lpfnWndProc=DefWindowProcW;
    wc.lpszClassName=L"hdd-noise.TrayDiagnostic";
    if(!RegisterClassW(&wc))return 5;
    g_window=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"hdd-noise tray diagnostic",
        WS_POPUP,-32000,-32000,1,1,nullptr,nullptr,g_instance,nullptr);
    // Use a distinct identity so diagnostics never replace a running app icon.
    g_trayGuid.Data1^=GetCurrentProcessId();g_trayGuid.Data4[7]=0xed;
    const bool icons=g_window&&LoadAppIcons();
    if(icons)SetTray(true);
    const bool added=g_trayAdded;
    std::ofstream out(path,std::ios::binary);
    out<<"processIntegrity="<<ProcessIntegrityRid()<<"\nicons="<<icons<<"\ntrayAdded="<<added
       <<"\nversion4="<<g_trayVersion4<<"\nwin32Error="<<g_trayError<<"\n";
    SetTray(false);
    if(g_window)DestroyWindow(g_window);g_window=nullptr;
    if(g_icon)DestroyIcon(g_icon);g_icon=nullptr;
    if(g_smallIcon)DestroyIcon(g_smallIcon);g_smallIcon=nullptr;
    return added&&out?0:5;
}
int MonitorDiagnostic(const std::filesystem::path& path) {
    hdd::DiskMonitor monitor;monitor.start();hdd::DiskSample sample;
    for(int i=0;i<50;++i){Sleep(100);sample=monitor.sample();if(sample.available)break;}
    auto disks=monitor.disks();monitor.stop();
    std::ofstream f(path,std::ios::binary);f<<"available="<<sample.available<<"\nbusy="<<sample.busy<<"\ntps="<<sample.transfersPerSecond<<"\nbytes="<<sample.bytesPerSecond<<"\ndisks="<<disks.size()<<"\n";
    return sample.available&&f?0:4;
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv&&argc>=2){std::wstring arg=argv[1];
        if(arg==L"--render-demo"){int result=RenderDemo(argc>=3?argv[2]:L"hdd-noise-demo.wav",argc>=4?argv[3]:L"wd");LocalFree(argv);return result;}
        if(arg==L"--diagnose-tray"){int result=TrayDiagnostic(argc>=3?argv[2]:L"tray-diagnostic.txt");LocalFree(argv);return result;}
        if(arg==L"--diagnose-monitor"){int result=MonitorDiagnostic(argc>=3?argv[2]:L"monitor-diagnostic.txt");LocalFree(argv);return result;}
    }
    bool trayOnly=argv&&argc>=2&&std::wstring(argv[1])==L"--tray";if(argv)LocalFree(argv);
    HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\hdd-noise.Instance.v1");
    if(!mutex)return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS){HWND existing=FindWindowW(kClass,nullptr);if(existing){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}CloseHandle(mutex);return 0;}
    g_instance=instance;
    if(auto user=GetModuleHandleW(L"user32.dll")){auto dpi=reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(GetProcAddress(user,"SetProcessDpiAwarenessContext"));if(dpi)dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);}
    if(auto user=GetModuleHandleW(L"user32.dll")){auto dpi=reinterpret_cast<UINT(WINAPI*)()>(GetProcAddress(user,"GetDpiForSystem"));if(dpi)g_scale=dpi()/96.0;}
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES|ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    MakeFonts();
    if(!LoadAppIcons()){MessageBoxW(nullptr,L"无法加载内置硬盘图标，请重新编译程序。",kTitle,MB_OK|MB_ICONERROR);CloseHandle(mutex);return 1;}
    g_taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.hIcon=g_icon;wc.hIconSm=g_smallIcon;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);wc.lpszClassName=kClass;
    if(!RegisterClassExW(&wc)){CloseHandle(mutex);return 1;}
    LoadSettings();if(g_settings.master>0)g_unmuteVolume=g_settings.master;g_audio.setSettings(g_settings);g_audio.setRunning(true);g_audio.start();g_monitor.select(g_diskId);g_monitor.start();
    RECT r{0,0,P(kWidth),P(kHeight)};DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    AdjustWindowRectEx(&r,style,FALSE,0);
    HWND window=CreateWindowExW(WS_EX_CONTROLPARENT,kClass,kTitle,style,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);
    if(!window){g_audio.stop();g_monitor.stop();CloseHandle(mutex);return 1;}
    SetTray(true);
    if(!trayOnly||!g_trayAdded)ShowWindow(window,show);UpdateWindow(window);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    g_audio.stop();g_monitor.stop();
    if(g_font)DeleteObject(g_font);if(g_icon)DestroyIcon(g_icon);if(g_smallIcon)DestroyIcon(g_smallIcon);CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
