#include "lang.h"

#include <windows.h>

namespace {

struct Entry {
    const wchar_t* zh;
    const wchar_t* en;
};

// Indexed by Str; keep in the same order as the enum.
const Entry kStrings[] = {
    /* AppName */ {L"任务栏性能监控", L"Taskbar Monitor"},
    /* TrayHidden */
    {L"任务栏空间不足，指标条已暂时隐藏",
     L"The taskbar is full, so the strip is hidden for now"},
    /* MenuDetails */ {L"性能详情", L"Performance details"},
    /* MenuTaskMgr */ {L"打开任务管理器", L"Open Task Manager"},
    /* MenuSettings */ {L"显示项目…", L"Display items…"},
    /* MenuPawnIo */
    {L"CPU 温度/功耗需要 PawnIO 驱动（打开下载页）",
     L"CPU temperature/power needs the PawnIO driver (open download page)"},
    /* MenuElevate */
    {L"以管理员身份重启（显示 CPU 温度/功耗）",
     L"Restart as administrator (shows CPU temperature/power)"},
    /* MenuUnsupported */
    {L"CPU 温度/功耗：此 CPU 暂不支持",
     L"CPU temperature/power: not supported on this CPU"},
    /* MenuAutostart */ {L"开机自动启动", L"Start with Windows"},
    /* MenuLanguage */ {L"语言 / Language", L"Language / 语言"},
    /* MenuLangAuto */ {L"跟随系统", L"Same as Windows"},
    /* MenuExit */ {L"退出", L"Exit"},
    /* SetTitle */ {L"任务栏监控 - 显示项目", L"Taskbar Monitor - Display items"},
    /* SetHint */
    {L"勾选后立即生效；至少保留一项。",
     L"Changes apply immediately; at least one item stays on."},
    /* GroupMemory */ {L"内存", L"Memory"},
    /* GroupNetwork */ {L"网络", L"Network"},
    /* ItemUtil */ {L"利用率", L"Utilization"},
    /* ItemTemp */ {L"温度", L"Temperature"},
    /* ItemFreq */ {L"频率", L"Clock"},
    /* ItemPower */ {L"功耗", L"Power"},
    /* ItemMemUtil */ {L"占用率", L"Usage"},
    /* ItemMemUsed */ {L"已用容量", L"Used"},
    /* ItemUp */ {L"上传", L"Upload"},
    /* ItemDown */ {L"下载", L"Download"},
    /* PanelTitle */ {L"性能监控", L"Performance"},
    /* PanelTaskMgr */ {L"任务管理器", L"Task Manager"},
    /* PanelSettings */ {L"显示项目", L"Display items"},
    /* Freq */ {L"频率", L"Clock"},
    /* Temp */ {L"温度", L"Temperature"},
    /* Power */ {L"功耗", L"Power"},
    /* Vram */ {L"显存", L"VRAM"},
    /* Cores */ {L"逻辑处理器负载", L"Logical processors"},
    /* NoGpu */ {L"未检测到 NVIDIA 显卡", L"No NVIDIA GPU detected"},
    /* Memory */ {L"内存", L"Memory"},
    /* InUse */ {L"已用", L"In use"},
    /* Available */ {L"可用", L"Available"},
    /* Committed */ {L"已提交", L"Committed"},
    /* Disk */ {L"磁盘", L"Disk"},
    /* AllDisks */ {L"所有物理磁盘", L"All physical disks"},
    /* ReadFmt */ {L"读 %ls", L"R %ls"},
    /* WriteFmt */ {L"写 %ls", L"W %ls"},
    /* ReadPeak */ {L"读取峰值 (60 秒)", L"Read peak (60 s)"},
    /* WritePeak */ {L"写入峰值 (60 秒)", L"Write peak (60 s)"},
    /* Network */ {L"网络", L"Network"},
    /* AllAdapters */ {L"物理网卡合计", L"All physical adapters"},
    /* DownPeak */ {L"下载峰值", L"Download peak"},
    /* UpPeak */ {L"上传峰值", L"Upload peak"},
    /* ByProcess */ {L"按进程查看 ›", L"By process ›"},
    /* Process */ {L"进程", L"Process"},
    /* Download */ {L"下载", L"Download"},
    /* Upload */ {L"上传", L"Upload"},
    /* NeedsAdmin */
    {L"按进程统计需要以管理员身份运行",
     L"Per-process statistics need administrator rights"},
    /* EtwFailed */ {L"无法启动网络事件跟踪", L"Couldn't start network event tracing"},
    /* Collecting */ {L"正在统计…", L"Collecting…"},
    /* NoActivity */ {L"暂无网络活动", L"No network activity"},
    /* NetTitle */ {L"网络活动", L"Network activity"},
    /* Back */ {L"‹ 总览", L"‹ Overview"},
    /* WhyPawnIo */ {L"需 PawnIO 驱动", L"Needs PawnIO"},
    /* WhyAdmin */ {L"需管理员权限", L"Needs admin"},
    /* WhyUnsupported */ {L"此 CPU 不支持", L"Not supported"},
};
static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == size_t(Str::Count), "string table out of sync with Str");

Lang g_pref = Lang::Auto;
bool g_english = false;

}  // namespace

void SetLang(Lang pref) {
    g_pref = pref;
    if (pref == Lang::Auto) g_english = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;
    else g_english = pref == Lang::English;
}

Lang GetLangPref() { return g_pref; }

const wchar_t* T(Str id) {
    const Entry& e = kStrings[int(id)];
    return g_english ? e.en : e.zh;
}
