#include "panel.h"

#include <dwmapi.h>
#include <initializer_list>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

#include "app.h"
#include "format.h"

namespace panel {
namespace {

constexpr wchar_t kClass[] = L"TaskbarMonitorPanel";

// Layout, in 96-dpi units.
constexpr int kPad = 16, kHeaderH = 24, kHeaderGap = 12;
constexpr int kSecHeadH = 44, kChartGap = 6, kStatsGap = 8, kStatsH = 36, kDivGap = 20;
constexpr int kCoresGap = 10, kCoresLabelH = 18, kCoresH = 28;
constexpr int kColW = 340, kColGap = 24;
constexpr int kNetW = 380, kNetChartH = 64, kListHeadH = 22, kRowH = 26, kRows = kTopProcesses;
constexpr int kOverviewChartH = 40;  // right column; the left column stretches its charts to match

HWND g_wnd;
Mode g_mode;
UINT g_dpi = 96;
RECT g_anchor;
ULONGLONG g_closedAt;  // when the panel last closed itself on deactivation
Mode g_closedMode;

struct Palette {
    COLORREF bg, text, text2, divider, chartBg, accent, accent2;
} g_pal;

struct Fonts {
    HFONT title, section, big, body, caption, small;
} g_f;

Shared g_snap;  // copy of the shared state taken for each paint

struct IconEntry {
    wchar_t path[MAX_PATH];
    HICON icon;
};
constexpr int kIconCache = 32;
IconEntry g_icons[kIconCache];
int g_iconCount;

enum Hit { HIT_NONE, HIT_TASKMGR, HIT_SETTINGS, HIT_NETWORK, HIT_OVERVIEW };
struct HitRect {
    RECT r;
    Hit id;
};
HitRect g_hits[8];
int g_hitCount;

int S(int v) { return MulDiv(v, g_dpi, 96); }

// ------------------------------------------------------------------ theme

COLORREF AccentShade(int index, COLORREF fallback) {
    // AccentPalette: 8 RGBA entries, light3 … dark3; index 3 is the accent itself.
    BYTE pal[32];
    DWORD size = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS &&
        size >= 32)
        return RGB(pal[index * 4], pal[index * 4 + 1], pal[index * 4 + 2]);
    return fallback;
}

void BuildPalette() {
    if (g_light)
        g_pal = {RGB(246, 246, 246), RGB(26, 26, 26),     RGB(96, 96, 96),    RGB(224, 224, 224),
                 RGB(234, 234, 234), AccentShade(4, RGB(0, 95, 184)), RGB(202, 94, 0)};
    else
        g_pal = {RGB(36, 36, 36),   RGB(255, 255, 255),   RGB(166, 166, 166), RGB(62, 62, 62),
                 RGB(46, 46, 46),   AccentShade(1, RGB(153, 235, 255)), RGB(255, 166, 80)};
}

void ApplyDwm(HWND hwnd) {
    const DWORD round = 2;  // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33, &round, sizeof(round));
    const BOOL dark = !g_light;  // DWMWA_USE_IMMERSIVE_DARK_MODE: border color
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
}

HFONT MakeFont(int px, int weight, const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    HFONT f = nullptr;
    const wchar_t* faces[] = {face, L"Segoe UI"};
    for (const wchar_t* fc : faces) {
        f = CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, fc);
        HGDIOBJ old = SelectObject(dc, f);
        wchar_t actual[LF_FACESIZE];
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, old);
        if (!lstrcmpiW(actual, fc) || fc != face) break;
        DeleteObject(f);
    }
    ReleaseDC(nullptr, dc);
    return f;
}

void DestroyFonts() {
    for (HFONT* f : {&g_f.title, &g_f.section, &g_f.big, &g_f.body, &g_f.caption, &g_f.small}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void CreateFonts() {
    DestroyFonts();
    g_f.title = MakeFont(16, FW_SEMIBOLD, L"Segoe UI Variable Display");
    g_f.section = MakeFont(14, FW_SEMIBOLD, L"Segoe UI Variable Text");
    g_f.big = MakeFont(22, FW_SEMIBOLD, L"Segoe UI Variable Display");
    g_f.body = MakeFont(13, FW_NORMAL, L"Segoe UI Variable Text");
    g_f.caption = MakeFont(12, FW_NORMAL, L"Segoe UI Variable Text");
    g_f.small = MakeFont(11, FW_NORMAL, L"Segoe UI Variable Small");
}

void DestroyIcons() {
    for (int i = 0; i < g_iconCount; ++i)
        if (g_icons[i].icon) DestroyIcon(g_icons[i].icon);
    g_iconCount = 0;
}

HICON IconFor(const wchar_t* path) {
    static HICON generic = LoadIconW(nullptr, IDI_APPLICATION);  // shared, never destroyed
    if (!path[0]) return generic;
    for (int i = 0; i < g_iconCount; ++i)
        if (!_wcsicmp(g_icons[i].path, path)) return g_icons[i].icon ? g_icons[i].icon : generic;
    if (g_iconCount == kIconCache) return generic;
    IconEntry& e = g_icons[g_iconCount++];
    wcscpy(e.path, path);
    e.icon = nullptr;
    if (FAILED(SHDefExtractIconW(path, 0, 0, &e.icon, nullptr, MAKELONG(S(16), 0)))) e.icon = nullptr;
    return e.icon ? e.icon : generic;
}

// ------------------------------------------------------------------ geometry

SIZE PanelSize() {
    if (g_mode == Mode::Overview) {
        const int section = kSecHeadH + kChartGap + kOverviewChartH + kStatsGap + kStatsH;
        const int column = 3 * section + 2 * kDivGap;
        return {S(2 * kPad + 2 * kColW + kColGap), S(2 * kPad + kHeaderH + kHeaderGap + column)};
    }
    const int h = kPad + kHeaderH + kHeaderGap + kSecHeadH + kChartGap + kNetChartH + kStatsGap + kStatsH + kDivGap +
                  kListHeadH + kRows * kRowH + kPad;
    return {S(2 * kPad + kNetW), S(h)};
}

RECT PanelRect(SIZE sz) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&g_anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    const int margin = S(12);
    int x = (g_anchor.left + g_anchor.right) / 2 - sz.cx / 2;
    if (x + sz.cx > wa.right - margin) x = wa.right - margin - sz.cx;
    if (x < wa.left + margin) x = wa.left + margin;
    int y = wa.bottom - margin - sz.cy;
    if (y < wa.top + margin) y = wa.top + margin;
    return {x, y, x + sz.cx, y + sz.cy};
}

// ------------------------------------------------------------------ drawing helpers

void Fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    if (w <= 0 || h <= 0) return;
    SetDCBrushColor(dc, c);
    RECT r = {x, y, x + w, y + h};
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Text(HDC dc, HFONT f, COLORREF c, const wchar_t* s, RECT r, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER);
}

int TextW(HDC dc, HFONT f, const wchar_t* s) {
    SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s, int(wcslen(s)), &sz);
    return sz.cx;
}

void AddHit(RECT r, Hit id) {
    if (g_hitCount < int(sizeof(g_hits) / sizeof(g_hits[0]))) g_hits[g_hitCount++] = {r, id};
}

// A text link, right-aligned so that it ends at `right`; returns its left edge.
int Link(HDC dc, int right, int y, int h, const wchar_t* s, Hit id) {
    const int w = TextW(dc, g_f.caption, s);
    RECT r = {right - w, y, right, y + h};
    Text(dc, g_f.caption, g_pal.accent, s, r, DT_RIGHT);
    AddHit(r, id);
    return right - w;
}

void SectionHead(HDC dc, int x, int y, int w, const wchar_t* title, const wchar_t* sub, const wchar_t* big,
                 COLORREF bigColor, const wchar_t* bigSub = L"", COLORREF bigSubColor = 0) {
    int rightW = TextW(dc, g_f.big, big);
    if (bigSub[0]) {
        int sw = TextW(dc, g_f.caption, bigSub);
        if (sw > rightW) rightW = sw;
    }
    const int textRight = x + w - rightW - S(12);
    Text(dc, g_f.section, g_pal.text, title, {x, y, textRight, y + S(20)}, DT_LEFT | DT_END_ELLIPSIS);
    Text(dc, g_f.caption, g_pal.text2, sub, {x, y + S(22), textRight, y + S(40)}, DT_LEFT | DT_END_ELLIPSIS);
    Text(dc, g_f.big, bigColor, big, {x + w - rightW, y - S(2), x + w, y + S(28)}, DT_RIGHT);
    if (bigSub[0])
        Text(dc, g_f.caption, bigSubColor, bigSub, {x + w - rightW, y + S(27), x + w, y + S(43)}, DT_RIGHT);
}

struct Stat {
    const wchar_t* label;
    const wchar_t* value;
};

void Stats(HDC dc, int x, int y, int w, const Stat* s, int n) {
    const int cw = w / n;
    for (int i = 0; i < n; ++i) {
        const int cx = x + i * cw;
        Text(dc, g_f.caption, g_pal.text2, s[i].label, {cx, y, cx + cw - S(4), y + S(16)}, DT_LEFT | DT_END_ELLIPSIS);
        Text(dc, g_f.body, g_pal.text, s[i].value, {cx, y + S(17), cx + cw - S(4), y + S(36)}, DT_LEFT | DT_END_ELLIPSIS);
    }
}

// Chart scale for a byte rate: the smallest 1/2/5 step that holds `v`, in the
// same 1024-based units FmtRate uses, so the label reads "2 MB/s", not "1.91 MB/s".
double NiceRate(double v) {
    for (double unit = 1024; unit < 1e15; unit *= 1024)
        for (double m : {1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0})
            if (m * unit >= v) return m * unit;
    return v;
}

// Drawn after the bars, on its own backing, so tall bars never hide it.
void ScaleLabel(HDC dc, int x, int y, const wchar_t* label) {
    const int tw = TextW(dc, g_f.small, label);
    Fill(dc, x, y, tw + S(10), S(16), g_pal.chartBg);
    Text(dc, g_f.small, g_pal.text2, label, {x + S(5), y + S(1), x + tw + S(10), y + S(16)}, DT_LEFT);
}

// Columns for the last kHistory seconds, newest on the right. `dir` = -1 grows
// up from `base`, +1 grows down from it.
void Bars(HDC dc, int x, int w, int base, int span, int dir, const History& hist, double scale, COLORREF c) {
    const double slot = double(w) / kHistory;
    const int gap = slot >= 4 ? 1 : 0;
    for (int i = 0; i < hist.count; ++i) {
        const int s = kHistory - hist.count + i;
        const int x0 = x + int(s * slot), x1 = x + int((s + 1) * slot) - gap;
        double v = hist.At(i) / scale;
        if (v > 1) v = 1;
        const int bh = int(v * span + 0.5);
        if (dir < 0) Fill(dc, x0, base - bh, x1 - x0, bh, c);
        else Fill(dc, x0, base, x1 - x0, bh, c);
    }
}

void PercentChart(HDC dc, int x, int y, int w, int h, const History& hist) {
    Fill(dc, x, y, w, h, g_pal.chartBg);
    Bars(dc, x, w, y + h, h - S(18), -1, hist, 100.0, g_pal.accent);
    ScaleLabel(dc, x, y, L"100%");
}

// Two rate series around a shared center line: `a` (accent) above, `b` below.
void MirrorChart(HDC dc, int x, int y, int w, int h, const History& a, const History& b) {
    double peak = a.Max() > b.Max() ? a.Max() : b.Max();
    const double scale = NiceRate(peak);
    wchar_t label[16];
    FmtRate(label, scale);
    Fill(dc, x, y, w, h, g_pal.chartBg);
    const int mid = y + h / 2, half = h / 2 - S(2);
    Fill(dc, x, mid, w, 1, g_pal.divider);
    Bars(dc, x, w, mid, half, -1, a, scale, g_pal.accent);
    Bars(dc, x, w, mid + 1, half, +1, b, scale, g_pal.accent2);
    ScaleLabel(dc, x, y, label);
}

void Divider(HDC dc, int x, int y, int w) { Fill(dc, x, y + S(kDivGap) / 2, w, 1, g_pal.divider); }

// ------------------------------------------------------------------ sections

int CpuSection(HDC dc, int x, int y, int w, int chartH) {
    const Metrics& m = g_snap.m;
    wchar_t util[16], freq[16], temp[16], power[16], name[128];
    FmtPct(util, m.cpuUtil); FmtGHz(freq, m.cpuMHz); FmtTemp(temp, m.cpuTemp); FmtWatt(power, m.cpuPower);
    const wchar_t* why = nullptr;  // why temperature/power are missing
    switch (g_hwState) {
        case HwSensorState::NotInstalled: why = L"需 PawnIO 驱动"; break;           // 需 PawnIO 驱动
        case HwSensorState::NeedsAdmin: why = L"需管理员权限"; break;   // 需管理员权限
        case HwSensorState::Unsupported: why = L"此 CPU 不支持"; break;         // 此 CPU 不支持
        case HwSensorState::Ok: break;
    }
    if (why && m.cpuTemp < 0) wcscpy(temp, why);
    if (why && m.cpuPower < 0) wcscpy(power, why);
    wcscpy(name, g_collector.CpuName());
    for (size_t n = wcslen(name); n && name[n - 1] == L' '; --n) name[n - 1] = 0;
    SectionHead(dc, x, y, w, L"CPU", name, util, g_pal.accent);
    int cy = y + S(kSecHeadH + kChartGap);
    PercentChart(dc, x, cy, w, chartH, g_snap.hist[H_CPU]);
    cy += chartH + S(kStatsGap);
    const Stat st[] = {{L"频率", freq}, {L"温度", temp}, {L"功耗", power}};  // 频率 温度 功耗
    Stats(dc, x, cy, w, st, 3);
    cy += S(kStatsH + kCoresGap);

    // 逻辑处理器负载 · N
    wchar_t label[64];
    swprintf(label, 64, L"逻辑处理器负载 · %d", m.coreCount);
    Text(dc, g_f.caption, g_pal.text2, m.coreCount ? label : L"逻辑处理器负载",
         {x, cy, x + w, cy + S(kCoresLabelH)}, DT_LEFT);
    cy += S(kCoresLabelH);
    const int n = m.coreCount;
    if (n > 0) {
        const int gap = w / n >= S(6) ? S(2) : 1;
        const double bw = double(w + gap) / n;
        const int h = S(kCoresH);
        for (int i = 0; i < n; ++i) {
            const int x0 = x + int(i * bw), x1 = x + int((i + 1) * bw) - gap;
            Fill(dc, x0, cy, x1 - x0, h, g_pal.chartBg);
            const int fh = int(m.coreUtil[i] / 100.0 * h + 0.5);
            Fill(dc, x0, cy + h - fh, x1 - x0, fh, g_pal.accent);
        }
    } else {
        Fill(dc, x, cy, w, S(kCoresH), g_pal.chartBg);
    }
    return cy + S(kCoresH);
}

int GpuSection(HDC dc, int x, int y, int w, int chartH) {
    const Metrics& m = g_snap.m;
    wchar_t util[16], freq[16], temp[16], power[16], used[16], total[16], vram[40];
    FmtPct(util, m.gpuUtil); FmtGHz(freq, m.gpuMHz); FmtTemp(temp, m.gpuTemp); FmtWatt(power, m.gpuPower);
    FmtGB(used, m.gpuMemUsedGB); FmtGB(total, m.gpuMemTotalGB);
    if (m.gpuMemUsedGB >= 0 && m.gpuMemTotalGB > 0)  // compact: "2.3/15.9 GB" fits a quarter column
        swprintf(vram, 40, L"%.1f/%ls", m.gpuMemUsedGB, total);
    else
        wcscpy(vram, used);
    const wchar_t* name = g_collector.GpuName()[0] ? g_collector.GpuName() : L"未检测到 NVIDIA 显卡";
    SectionHead(dc, x, y, w, L"GPU", name, util, g_pal.accent);
    int cy = y + S(kSecHeadH + kChartGap);
    PercentChart(dc, x, cy, w, chartH, g_snap.hist[H_GPU]);
    cy += chartH + S(kStatsGap);
    // 频率 温度 功耗 显存
    const Stat st[] = {{L"频率", freq}, {L"温度", temp}, {L"功耗", power}, {L"显存", vram}};
    Stats(dc, x, cy, w, st, 4);
    return cy + S(kStatsH);
}

int MemorySection(HDC dc, int x, int y, int w, int chartH) {
    const Metrics& m = g_snap.m;
    wchar_t util[16], used[16], total[16], avail[16], cu[16], cl[16], sub[48], commit[40];
    FmtPct(util, m.memUtil); FmtGB(used, m.memUsedGB); FmtGB(total, m.memTotalGB);
    FmtGB(avail, m.memTotalGB >= 0 ? m.memTotalGB - m.memUsedGB : -1);
    FmtGB(cu, m.commitUsedGB); FmtGB(cl, m.commitLimitGB);
    swprintf(sub, 48, L"%ls / %ls", used, total);
    swprintf(commit, 40, L"%ls / %ls", cu, cl);
    SectionHead(dc, x, y, w, L"内存", sub, util, g_pal.accent);  // 内存
    int cy = y + S(kSecHeadH + kChartGap);
    PercentChart(dc, x, cy, w, chartH, g_snap.hist[H_MEM]);
    cy += chartH + S(kStatsGap);
    // 已用 可用 已提交
    const Stat st[] = {{L"已用", used}, {L"可用", avail}, {L"已提交", commit}};
    Stats(dc, x, cy, w, st, 3);
    return cy + S(kStatsH);
}

int DiskSection(HDC dc, int x, int y, int w, int chartH) {
    const Metrics& m = g_snap.m;
    wchar_t rd[16], wr[16], big[24], bigSub[24];
    FmtRate(rd, m.diskRead); FmtRate(wr, m.diskWrite);
    swprintf(big, 24, L"读 %ls", rd);     // 读
    swprintf(bigSub, 24, L"写 %ls", wr);  // 写
    SectionHead(dc, x, y, w, L"磁盘", L"所有物理磁盘", big, g_pal.accent, bigSub,
                g_pal.accent2);  // 磁盘 / 所有物理磁盘
    int cy = y + S(kSecHeadH + kChartGap);
    MirrorChart(dc, x, cy, w, chartH, g_snap.hist[H_DISK_R], g_snap.hist[H_DISK_W]);
    cy += chartH + S(kStatsGap);
    wchar_t peakR[16], peakW[16];
    FmtRate(peakR, g_snap.hist[H_DISK_R].Max());
    FmtRate(peakW, g_snap.hist[H_DISK_W].Max());
    // 读取峰值 写入峰值 (60 秒)
    const Stat st[] = {{L"读取峰值 (60 秒)", peakR}, {L"写入峰值 (60 秒)", peakW}};
    Stats(dc, x, cy, w, st, 2);
    return cy + S(kStatsH);
}

int NetworkSection(HDC dc, int x, int y, int w, int chartH, bool withLink) {
    const Metrics& m = g_snap.m;
    wchar_t down[16], up[16], big[24], bigSub[24];
    FmtRate(down, m.netDown); FmtRate(up, m.netUp);
    swprintf(big, 24, L"↓ %ls", down);
    swprintf(bigSub, 24, L"↑ %ls", up);
    SectionHead(dc, x, y, w, L"网络", L"物理网卡合计", big, g_pal.accent, bigSub,
                g_pal.accent2);  // 网络 / 物理网卡合计
    int cy = y + S(kSecHeadH + kChartGap);
    MirrorChart(dc, x, cy, w, chartH, g_snap.hist[H_NET_DOWN], g_snap.hist[H_NET_UP]);
    cy += chartH + S(kStatsGap);
    wchar_t peakD[16], peakU[16];
    FmtRate(peakD, g_snap.hist[H_NET_DOWN].Max());
    FmtRate(peakU, g_snap.hist[H_NET_UP].Max());
    // 下载峰值 上传峰值
    const Stat st[] = {{L"下载峰值", peakD}, {L"上传峰值", peakU}};
    Stats(dc, x, cy, withLink ? w * 2 / 3 : w, st, 2);
    if (withLink)  // 按进程查看 ›
        Link(dc, x + w, cy + S(10), S(20), L"按进程查看 ›", HIT_NETWORK);
    return cy + S(kStatsH);
}

void ProcessList(HDC dc, int x, int y, int w) {
    const int upW = S(78), downW = S(78), colGap = S(8);
    const int upL = x + w - upW, downL = upL - colGap - downW, nameL = x + S(26), nameR = downL - colGap;
    // 进程 下载 上传
    Text(dc, g_f.caption, g_pal.text2, L"进程", {x, y, nameR, y + S(kListHeadH)}, DT_LEFT);
    Text(dc, g_f.caption, g_pal.text2, L"下载", {downL, y, downL + downW, y + S(kListHeadH)}, DT_RIGHT);
    Text(dc, g_f.caption, g_pal.text2, L"上传", {upL, y, x + w, y + S(kListHeadH)}, DT_RIGHT);
    y += S(kListHeadH);

    const wchar_t* message = nullptr;
    switch (g_snap.netState) {
        case netproc::State::NeedsAdmin:  // 按进程统计需要以管理员身份运行
            message = L"按进程统计需要以管理员身份运行";
            break;
        case netproc::State::Failed:  // 无法启动网络事件跟踪
            message = L"无法启动网络事件跟踪";
            break;
        case netproc::State::Off:
        case netproc::State::Running:
            if (!g_snap.topValid) message = L"正在统计…";  // 正在统计…
            else if (!g_snap.topCount) message = L"暂无网络活动";  // 暂无网络活动
            break;
    }
    if (message) {
        Text(dc, g_f.body, g_pal.text2, message, {x, y, x + w, y + S(kRowH) * 3}, DT_CENTER);
        return;
    }

    const double maxTotal = g_snap.top[0].down + g_snap.top[0].up;
    for (int i = 0; i < g_snap.topCount; ++i) {
        const netproc::Entry& e = g_snap.top[i];
        const int ry = y + i * S(kRowH);
        const int share = maxTotal > 0 ? int((e.down + e.up) / maxTotal * w) : 0;
        Fill(dc, x, ry + S(2), share, S(kRowH) - S(4), g_pal.chartBg);
        DrawIconEx(dc, x + S(4), ry + (S(kRowH) - S(16)) / 2, IconFor(e.path), S(16), S(16), 0, nullptr, DI_NORMAL);
        wchar_t name[80], down[16], up[16];
        if (e.processes > 1) swprintf(name, 80, L"%ls (%d)", e.name, e.processes);
        else wcscpy(name, e.name);
        FmtRate(down, e.down);
        FmtRate(up, e.up);
        Text(dc, g_f.body, g_pal.text, name, {nameL, ry, nameR, ry + S(kRowH)}, DT_LEFT | DT_END_ELLIPSIS);
        Text(dc, g_f.body, g_pal.accent, down, {downL, ry, downL + downW, ry + S(kRowH)}, DT_RIGHT);
        Text(dc, g_f.body, g_pal.accent2, up, {upL, ry, x + w, ry + S(kRowH)}, DT_RIGHT);
    }
}

// ------------------------------------------------------------------ pages

void PaintOverview(HDC dc, int w) {
    const int x0 = S(kPad), top = S(kPad);
    Text(dc, g_f.title, g_pal.text, L"性能监控", {x0, top, w / 2, top + S(kHeaderH)}, DT_LEFT);  // 性能监控
    int lx = Link(dc, w - S(kPad), top, S(kHeaderH), L"任务管理器", HIT_TASKMGR);  // 任务管理器
    Link(dc, lx - S(16), top, S(kHeaderH), L"显示项目", HIT_SETTINGS);  // 显示项目

    const int y0 = top + S(kHeaderH + kHeaderGap);
    const int colW = S(kColW);
    const int rightX = x0 + colW + S(kColGap);

    // Right column: three equal sections.
    const int chartR = S(kOverviewChartH);
    int y = MemorySection(dc, rightX, y0, colW, chartR);
    Divider(dc, rightX, y, colW);
    y = DiskSection(dc, rightX, y + S(kDivGap), colW, chartR);
    Divider(dc, rightX, y, colW);
    const int bottom = NetworkSection(dc, rightX, y + S(kDivGap), colW, chartR, true);

    // Left column: CPU and GPU, charts stretched so both columns end together.
    const int fixed = 2 * S(kSecHeadH + kChartGap + kStatsGap + kStatsH) + S(kCoresGap + kCoresLabelH + kCoresH) +
                      S(kDivGap);
    const int chartL = (bottom - y0 - fixed) / 2;
    y = CpuSection(dc, x0, y0, colW, chartL);
    Divider(dc, x0, y, colW);
    GpuSection(dc, x0, y + S(kDivGap), colW, chartL);
}

void PaintNetwork(HDC dc, int w) {
    const int x0 = S(kPad), top = S(kPad), cw = w - 2 * S(kPad);
    Text(dc, g_f.title, g_pal.text, L"网络活动", {x0, top, w / 2, top + S(kHeaderH)}, DT_LEFT);  // 网络活动
    Link(dc, w - S(kPad), top, S(kHeaderH), L"‹ 总览", HIT_OVERVIEW);  // ‹ 总览
    int y = NetworkSection(dc, x0, top + S(kHeaderH + kHeaderGap), cw, S(kNetChartH), false);
    Divider(dc, x0, y, cw);
    ProcessList(dc, x0, y + S(kDivGap), cw);
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g_f.body);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, 0, 0, rc.right, rc.bottom, g_pal.bg);

    AcquireSRWLockShared(&g_lock);
    g_snap = g_shared;
    ReleaseSRWLockShared(&g_lock);

    g_hitCount = 0;
    if (g_mode == Mode::Overview) PaintOverview(dc, rc.right);
    else PaintNetwork(dc, rc.right);

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

Hit HitTest(POINT pt) {
    for (int i = 0; i < g_hitCount; ++i)
        if (PtInRect(&g_hits[i].r, pt)) return g_hits[i].id;
    return HIT_NONE;
}

void SetWants() {
    const bool open = g_wnd != nullptr;
    InterlockedExchange(&g_wantPerCore, open && g_mode == Mode::Overview);
    InterlockedExchange(&g_wantNetProc, open && g_mode == Mode::Network);
    WakeWorker();
}

void Open(Mode mode);

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: Paint(hwnd); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                g_closedAt = GetTickCount64();
                g_closedMode = g_mode;
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_SYSCOMMAND:  // the taskbar button minimizes; a flyout just closes
            if ((wp & 0xFFF0) == SC_MINIMIZE) {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorW(nullptr, HitTest(pt) != HIT_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONUP: {
            const POINT pt = {short(LOWORD(lp)), short(HIWORD(lp))};
            switch (HitTest(pt)) {
                case HIT_TASKMGR: Close(); OpenTaskManager(); break;
                case HIT_SETTINGS: Close(); ShowSettings(); break;
                case HIT_NETWORK: Close(); Open(Mode::Network); break;
                case HIT_OVERVIEW: Close(); Open(Mode::Overview); break;
                case HIT_NONE: break;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            CreateFonts();
            DestroyIcons();
            const RECT r = PanelRect(PanelSize());
            SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_NCDESTROY:
            if (hwnd == g_wnd) {
                g_wnd = nullptr;
                DestroyFonts();
                DestroyIcons();
                SetWants();
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Open(Mode mode) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_mode = mode;
    g_dpi = g_taskbar ? GetDpiForWindow(g_taskbar) : 96;
    if (!g_dpi) g_dpi = 96;
    BuildPalette();
    CreateFonts();
    const RECT r = PanelRect(PanelSize());
    // WS_EX_APPWINDOW: a taskbar button while the panel is open, so it is
    // visible (and closable from the taskbar) like any other window.
    // 任务栏性能监控
    g_wnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST, kClass, L"任务栏性能监控",
                            WS_POPUP | WS_SYSMENU, r.left, r.top, r.right - r.left, r.bottom - r.top, g_host, nullptr,
                            g_inst, nullptr);
    if (!g_wnd) {
        DestroyFonts();
        return;
    }
    ApplyDwm(g_wnd);
    SendMessageW(g_wnd, WM_SETICON, ICON_SMALL, LPARAM(AppIcon(GetSystemMetricsForDpi(SM_CXSMICON, g_dpi))));
    SendMessageW(g_wnd, WM_SETICON, ICON_BIG, LPARAM(AppIcon(GetSystemMetricsForDpi(SM_CXICON, g_dpi))));
    SetWants();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

}  // namespace

void Toggle(Mode mode, const RECT& anchor) {
    if (g_wnd) {
        const bool same = g_mode == mode;
        Close();
        if (same) return;
    } else if (GetTickCount64() - g_closedAt < 400 && g_closedMode == mode) {
        return;  // this click is what just closed it (it deactivated the panel first)
    }
    g_anchor = anchor;
    Open(mode);
}

void Close() {
    if (g_wnd) DestroyWindow(g_wnd);
}

bool IsOpen() { return g_wnd != nullptr; }

void OnData() {
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

void OnThemeChanged() {
    if (!g_wnd) return;
    BuildPalette();
    ApplyDwm(g_wnd);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

}  // namespace panel
