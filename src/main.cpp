// TaskbarMonitor: a small strip of system metrics drawn inside the Windows 11
// taskbar, immediately left of the notification area (the "^" overflow button).
//
// Threads: the UI thread owns the windows and only renders; a worker thread
// samples metrics once per second, so a slow sensor can never stall the
// taskbar (our child window shares explorer's input queue).

#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#include "app.h"
#include "format.h"
#include "lang.h"
#include "netproc.h"
#include "panel.h"
#include "tbspace.h"

// ------------------------------------------------------------------ shared state (app.h)

SRWLOCK g_lock = SRWLOCK_INIT;
Shared g_shared;
HINSTANCE g_inst;
HWND g_host, g_taskbar;
Collector g_collector;
HwSensorState g_hwState = HwSensorState::Unsupported;
bool g_light;
volatile LONG g_wantPerCore, g_wantNetProc;

namespace {

// Asking UI Automation where the taskbar's items end costs a cross-process
// round trip into explorer (~10 ms of CPU), so it only happens after something
// that can move them: a top-level window appearing or disappearing (the same
// shell-hook notifications the taskbar itself acts on), a settings or display
// change, or the notification area changing width. The count covers the
// taskbar's own animation by checking a few consecutive ticks.
volatile LONG g_taskbarDirty = 3;
void MarkTaskbarDirty() { InterlockedExchange(&g_taskbarDirty, 3); }

constexpr wchar_t kAppName[] = L"TaskbarMonitor";
constexpr wchar_t kHostClass[] = L"TaskbarMonitorHost";
constexpr wchar_t kStripClass[] = L"TaskbarMonitorStrip";
constexpr UINT WM_APP_METRICS = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;  // notification-area icon callback
constexpr UINT kTrayId = 1;
constexpr DWORD kIntervalMs = 1000;

enum MenuId {
    ID_TASKMGR = 1, ID_DETAILS, ID_SETTINGS, ID_PAWNIO, ID_ELEVATE, ID_AUTOSTART, ID_EXIT,
    ID_LANG_AUTO, ID_LANG_ZH, ID_LANG_EN,  // same order as Lang
};

HWND g_strip;
bool g_stripHidden;  // collapsed away because the taskbar has no room
UINT g_msgTaskbarCreated, g_msgQuit, g_msgShow, g_msgLang, g_msgShellHook;
HANDLE g_mutex, g_stopEvent, g_wakeEvent, g_worker;

}  // namespace

// ------------------------------------------------------------------ utilities

void WakeWorker() {
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

bool IsElevated() {
    HANDLE token;
    TOKEN_ELEVATION e = {};
    DWORD size;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size);
        CloseHandle(token);
    }
    return e.TokenIsElevated != 0;
}

HICON AppIcon(int size) {
    return static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_SHARED));
}

namespace {

bool ReadLightTheme() {
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

void ExePath(wchar_t* buf) { GetModuleFileNameW(nullptr, buf, MAX_PATH); }

// Runs a console tool without a window and returns its exit code.
DWORD RunHidden(wchar_t* cmdline) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, cmdline, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return DWORD(-1);
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// ------------------------------------------------------------------ autostart
// Elevated: a logon task with highest privileges, so temperature/power work
// at sign-in without a UAC prompt. Otherwise: the per-user Run key.

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool TaskExists() {
    wchar_t cmd[] = L"schtasks.exe /Query /TN TaskbarMonitor";
    return RunHidden(cmd) == 0;
}

bool RunKeyExists() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool CreateLogonTask() {
    wchar_t exe[MAX_PATH], user[256], xmlPath[MAX_PATH];
    ExePath(exe);
    DWORD n = 256;
    if (!GetEnvironmentVariableW(L"USERDOMAIN", user, 256)) user[0] = 0;
    size_t len = wcslen(user);
    if (len) user[len++] = L'\\';
    n = DWORD(256 - len);
    GetUserNameW(user + len, &n);

    static wchar_t xml[4096];
    swprintf(xml, 4096,
             L"\xFEFF<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
             L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
             L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%ls</UserId></LogonTrigger></Triggers>\r\n"
             L"<Principals><Principal id=\"Author\"><UserId>%ls</UserId><LogonType>InteractiveToken</LogonType>"
             L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
             L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
             L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
             L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
             L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings>\r\n"
             L"<Actions Context=\"Author\"><Exec><Command>\"%ls\"</Command></Exec></Actions>\r\n"
             L"</Task>\r\n",
             user, user, exe);

    GetTempPathW(MAX_PATH, xmlPath);
    wcscat(xmlPath, L"TaskbarMonitor.xml");
    HANDLE f = CreateFileW(xmlPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(f, xml, DWORD(wcslen(xml) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);

    wchar_t cmd[MAX_PATH + 64];
    swprintf(cmd, MAX_PATH + 64, L"schtasks.exe /Create /F /TN TaskbarMonitor /XML \"%ls\"", xmlPath);
    bool ok = RunHidden(cmd) == 0;
    DeleteFileW(xmlPath);
    return ok;
}

bool IsAutostart() { return RunKeyExists() || TaskExists(); }

void SetAutostart(bool on) {
    if (on) {
        if (IsElevated() && CreateLogonTask()) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);
            return;
        }
        wchar_t exe[MAX_PATH + 2] = L"\"";
        ExePath(exe + 1);
        wcscat(exe, L"\"");
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName, REG_SZ, exe, DWORD((wcslen(exe) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);
        wchar_t cmd[] = L"schtasks.exe /Delete /F /TN TaskbarMonitor";
        if (TaskExists()) RunHidden(cmd);
    }
}

// ------------------------------------------------------------------ display items
// Each metric can be shown or hidden individually. Within a group, enabled
// items fill two-row sub-columns in this order (column-major), so with
// everything on a group reads "util freq / temp power".

enum Item {
    IT_CPU_UTIL, IT_CPU_TEMP, IT_CPU_FREQ, IT_CPU_POWER,
    IT_GPU_UTIL, IT_GPU_TEMP, IT_GPU_FREQ, IT_GPU_POWER,
    IT_MEM_UTIL, IT_MEM_USED,
    IT_NET_UP, IT_NET_DOWN,
    IT_COUNT
};
constexpr int kNumGroups = 4;
constexpr DWORD kAllItems = (1u << IT_COUNT) - 1;

struct ItemDef {
    int group;
    const wchar_t* prefix;  // drawn left-aligned in the cell, e.g. the network arrows
    const wchar_t* tmpl;    // worst-case value, fixes the cell width
    Str name;               // settings window label
};
const ItemDef kItems[IT_COUNT] = {
    {0, L"", L"100%", Str::ItemUtil},
    {0, L"", L"100°C", Str::ItemTemp},
    {0, L"", L"8.88 GHz", Str::ItemFreq},
    {0, L"", L"888 W", Str::ItemPower},
    {1, L"", L"100%", Str::ItemUtil},
    {1, L"", L"100°C", Str::ItemTemp},
    {1, L"", L"8.88 GHz", Str::ItemFreq},
    {1, L"", L"888 W", Str::ItemPower},
    {2, L"", L"100%", Str::ItemMemUtil},
    {2, L"", L"88.8 GB", Str::ItemMemUsed},
    {3, L"↑", L"9.99 MB/s", Str::ItemUp},
    {3, L"↓", L"9.99 MB/s", Str::ItemDown},
};
const wchar_t* const kGroupLabel[kNumGroups] = {L"CPU", L"GPU", L"RAM", L""};

const wchar_t* GroupName(int g) {
    switch (g) {
        case 0: return L"CPU";
        case 1: return L"GPU";
        case 2: return T(Str::GroupMemory);
        default: return T(Str::GroupNetwork);
    }
}

DWORD g_items = kAllItems;  // bit per Item, persisted in the registry

// Groups given up first when the taskbar runs out of room: GPU, RAM, network, CPU.
const int kCollapseOrder[kNumGroups] = {1, 2, 3, 0};

DWORD GroupMask(int group) {
    DWORD mask = 0;
    for (int i = 0; i < IT_COUNT; ++i)
        if (kItems[i].group == group) mask |= 1u << i;
    return mask;
}

constexpr wchar_t kSettingsKey[] = L"Software\\TaskbarMonitor";

void LoadSettings() {
    DWORD v, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Items", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS &&
        (v & kAllItems))
        g_items = v & kAllItems;
    size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Language", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS ||
        v > DWORD(Lang::English))
        v = DWORD(Lang::Auto);
    SetLang(Lang(v));
}

void SaveSettings() {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Items", REG_DWORD, &g_items, sizeof(g_items));
    const DWORD lang = DWORD(GetLangPref());
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Language", REG_DWORD, &lang, sizeof(lang));
}

void ItemText(int item, const Metrics& m, wchar_t* b) {
    switch (item) {
        case IT_CPU_UTIL: FmtPct(b, m.cpuUtil); break;
        case IT_CPU_TEMP: FmtTemp(b, m.cpuTemp); break;
        case IT_CPU_FREQ: FmtGHz(b, m.cpuMHz); break;
        case IT_CPU_POWER: FmtWatt(b, m.cpuPower); break;
        case IT_GPU_UTIL: FmtPct(b, m.gpuUtil); break;
        case IT_GPU_TEMP: FmtTemp(b, m.gpuTemp); break;
        case IT_GPU_FREQ: FmtGHz(b, m.gpuMHz); break;
        case IT_GPU_POWER: FmtWatt(b, m.gpuPower); break;
        case IT_MEM_UTIL: FmtPct(b, m.memUtil); break;
        case IT_MEM_USED: FmtGB(b, m.memUsedGB); break;
        case IT_NET_UP: FmtRate(b, m.netUp); break;
        case IT_NET_DOWN: FmtRate(b, m.netDown); break;
        default: b[0] = 0;
    }
}

// ------------------------------------------------------------------ layout
// Widths come from worst-case templates, so the strip only changes width when
// the item selection changes, and values are right-aligned so digits don't jitter.

constexpr int kCentered = -1;  // row index for a cell/label centered vertically

struct Cell {
    int item;
    int left, right;  // prefix drawn at left, value right-aligned at right
    int row;          // 0, 1 or kCentered
};
struct Label {
    const wchar_t* text;
    int x, row;
};

struct Layout {
    UINT dpi = 0;
    DWORD items = 0;
    HFONT font = nullptr;
    int width = 0, lineH = 0;
    int pad, colGap, labelGap, fieldGap;
    Cell cells[IT_COUNT];
    int ncells = 0;
    Label labels[kNumGroups];
    int nlabels = 0;
    int groupL[kNumGroups], groupR[kNumGroups];  // x extent per group, for clicks; -1 if absent
} g_lay;

int Scale(int v) { return MulDiv(v, g_lay.dpi, 96); }

int TextW(HDC dc, const wchar_t* s) {
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s, int(wcslen(s)), &sz);
    return sz.cx;
}

void CreateLayoutFont(HDC dc) {
    if (g_lay.font) DeleteObject(g_lay.font);
    g_lay.font = nullptr;
    // The taskbar clock's typeface on Windows 11; Segoe UI where it's missing.
    static const wchar_t* const kFaces[] = {L"Segoe UI Variable Text", L"Segoe UI"};
    for (const wchar_t* face : kFaces) {
        HFONT f = CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        HGDIOBJ prev = SelectObject(dc, f);
        wchar_t actual[LF_FACESIZE];
        GetTextFaceW(dc, LF_FACESIZE, actual);
        SelectObject(dc, prev);
        if (lstrcmpiW(actual, face) == 0) {
            g_lay.font = f;
            return;
        }
        DeleteObject(f);
    }
    g_lay.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

void BuildLayout(UINT dpi, DWORD items) {
    if (g_lay.font && g_lay.dpi == dpi && g_lay.items == items) return;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!g_lay.font || g_lay.dpi != dpi) {
        g_lay.dpi = dpi;
        CreateLayoutFont(dc);
    }
    g_lay.items = items;
    g_lay.pad = Scale(4);
    g_lay.colGap = Scale(12);
    g_lay.labelGap = Scale(5);
    g_lay.fieldGap = Scale(6);

    HGDIOBJ old = SelectObject(dc, g_lay.font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    g_lay.lineH = tm.tmHeight;
    g_lay.ncells = g_lay.nlabels = 0;

    int x = g_lay.pad;
    bool first = true;
    for (int g = 0; g < kNumGroups; ++g) {
        g_lay.groupL[g] = g_lay.groupR[g] = -1;
        int list[IT_COUNT], n = 0;
        for (int i = 0; i < IT_COUNT; ++i)
            if (kItems[i].group == g && (items & (1u << i))) list[n++] = i;
        if (!n) continue;
        if (!first) x += g_lay.colGap;
        first = false;
        g_lay.groupL[g] = x;
        if (kGroupLabel[g][0]) {
            g_lay.labels[g_lay.nlabels++] = {kGroupLabel[g], x, n == 1 ? kCentered : 0};
            x += TextW(dc, kGroupLabel[g]) + g_lay.labelGap;
        }
        for (int k = 0; k < n; k += 2) {
            const int count = k + 1 < n ? 2 : 1;
            int subW = 0;
            for (int j = 0; j < count; ++j) {
                const ItemDef& d = kItems[list[k + j]];
                int w = TextW(dc, d.tmpl) + (d.prefix[0] ? TextW(dc, d.prefix) + g_lay.labelGap : 0);
                if (w > subW) subW = w;
            }
            for (int j = 0; j < count; ++j)
                g_lay.cells[g_lay.ncells++] = {list[k + j], x, x + subW, count == 1 ? kCentered : j};
            x += subW;
            if (k + 2 < n) x += g_lay.fieldGap;
        }
        g_lay.groupR[g] = x;
    }
    g_lay.width = x + g_lay.pad;
    SelectObject(dc, old);
    DeleteDC(dc);
}

// ------------------------------------------------------------------ rendering

// Text is drawn white-on-black with grayscale antialiasing; the coverage then
// becomes per-pixel alpha for UpdateLayeredWindow. Background alpha is 1 so
// the whole strip receives mouse input while staying invisible.
void Render() {
    if (!g_strip || !g_taskbar) return;

    RECT client;
    GetClientRect(g_taskbar, &client);
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return;
    RECT trayRc;
    GetWindowRect(tray, &trayRc);
    MapWindowPoints(HWND_DESKTOP, g_taskbar, reinterpret_cast<POINT*>(&trayRc), 2);
    static LONG lastTrayLeft = LONG_MIN;
    if (trayRc.left != lastTrayLeft) {  // tray icons added/removed, taskbar resized
        lastTrayLeft = trayRc.left;
        MarkTaskbarDirty();
    }

    // Room left by the taskbar's own items (task buttons, widgets…). When it
    // is too small, give up whole groups, least important first, and finally
    // hide; everything comes back once the taskbar frees the space again.
    const UINT dpi = GetDpiForWindow(g_taskbar);
    AcquireSRWLockShared(&g_lock);
    const int occupied = g_shared.occupiedRight;
    ReleaseSRWLockShared(&g_lock);
    int avail = INT_MAX;
    if (occupied >= 0) {
        POINT p = {occupied, 0};
        ScreenToClient(g_taskbar, &p);
        avail = trayRc.left - p.x - MulDiv(8, dpi, 96);
    }
    DWORD items = g_items;
    BuildLayout(dpi, items);
    for (int k = 0; g_lay.width > avail && k < kNumGroups; ++k) {
        items &= ~GroupMask(kCollapseOrder[k]);
        if (!items) break;
        BuildLayout(dpi, items);
    }
    g_stripHidden = !items || g_lay.width > avail;
    if (g_stripHidden) {
        if (IsWindowVisible(g_strip)) ShowWindow(g_strip, SW_HIDE);
        return;
    }

    // Span the notification area's own vertical extent: on touch devices the
    // taskbar window can be taller than the visible bar (cf. TrafficMonitor).
    int y = trayRc.top, h = trayRc.bottom - trayRc.top;
    if (h <= 0 || y < client.top || trayRc.bottom > client.bottom) {
        y = client.top;
        h = client.bottom - client.top;
    }
    const int w = g_lay.width;
    if (w <= 0 || h <= 0) return;

    SetWindowPos(g_strip, HWND_TOP, trayRc.left - w, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, g_lay.font);
    SetBkMode(dc, TRANSPARENT);
    // Draw in the polarity the strip is displayed in (dark-on-light or
    // light-on-dark) so GDI's antialiasing gamma matches the real background.
    if (g_light) memset(bits, 0xFF, size_t(w) * h * 4);
    const COLORREF labelColor = g_light ? RGB(80, 80, 80) : RGB(175, 175, 175);  // ~70% opacity
    const COLORREF valueColor = g_light ? RGB(0, 0, 0) : RGB(255, 255, 255);

    AcquireSRWLockShared(&g_lock);
    const Metrics m = g_shared.m;
    const bool haveMetrics = g_shared.have;
    ReleaseSRWLockShared(&g_lock);

    const int top = (h - 2 * g_lay.lineH) / 2;
    auto rowY = [&](int row) { return row == kCentered ? (h - g_lay.lineH) / 2 : top + row * g_lay.lineH; };

    SetTextColor(dc, labelColor);
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    for (int i = 0; i < g_lay.nlabels; ++i) {
        const Label& l = g_lay.labels[i];
        TextOutW(dc, l.x, rowY(l.row), l.text, int(wcslen(l.text)));
    }
    for (int i = 0; i < g_lay.ncells; ++i) {
        const Cell& c = g_lay.cells[i];
        const wchar_t* prefix = kItems[c.item].prefix;
        if (prefix[0]) TextOutW(dc, c.left, rowY(c.row), prefix, int(wcslen(prefix)));
    }
    if (haveMetrics) {
        SetTextColor(dc, valueColor);
        SetTextAlign(dc, TA_RIGHT | TA_TOP | TA_NOUPDATECP);
        for (int i = 0; i < g_lay.ncells; ++i) {
            const Cell& c = g_lay.cells[i];
            wchar_t s[16];
            ItemText(c.item, m, s);
            TextOutW(dc, c.right, rowY(c.row), s, int(wcslen(s)));
        }
    }
    GdiFlush();

    const BYTE fg = g_light ? 0 : 255;
    DWORD* px = static_cast<DWORD*>(bits);
    for (int i = 0, n = w * h; i < n; ++i) {
        DWORD p = px[i];
        BYTE r = BYTE(p >> 16), g = BYTE(p >> 8), b = BYTE(p);
        BYTE a = r > g ? (r > b ? r : b) : (g > b ? g : b);  // coverage of light text
        if (g_light) a = BYTE(255 - (r < g ? (r < b ? r : b) : (g < b ? g : b)));  // of dark text
        BYTE c = BYTE(fg * a / 255);
        if (a == 0) a = 1;
        px[i] = (DWORD(a) << 24) | (DWORD(c) << 16) | (DWORD(c) << 8) | c;
    }

    POINT src = {0, 0};
    SIZE size = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_strip, nullptr, nullptr, &size, dc, &src, 0, &bf, ULW_ALPHA);

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ------------------------------------------------------------------ settings window
// Modeless window of checkboxes; every click applies immediately.

constexpr wchar_t kSettingsClass[] = L"TaskbarMonitorSettings";
// Checkbox order (and tab order) in the window, which reads more naturally
// than the strip's column-major fill order.
const int kSettingsOrder[IT_COUNT] = {
    IT_CPU_UTIL, IT_CPU_FREQ, IT_CPU_TEMP, IT_CPU_POWER,
    IT_GPU_UTIL, IT_GPU_FREQ, IT_GPU_TEMP, IT_GPU_POWER,
    IT_MEM_UTIL, IT_MEM_USED,
    IT_NET_UP, IT_NET_DOWN,
};
constexpr int ID_ITEM_BASE = 100;
HWND g_settings;
HFONT g_settingsFont;

void LayoutSettings(HWND hwnd, UINT dpi) {
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    if (g_settingsFont) DeleteObject(g_settingsFont);
    g_settingsFont = CreateFontIndirectW(&ncm.lfMessageFont);

    // Wide enough for the English labels ("Temperature", "Utilization").
    const int margin = S(12), boxW = S(440), checkW = S(104), checkH = S(22), boxH = S(52), gap = S(8);
    int y = margin;
    for (int g = 0; g < kNumGroups; ++g) {
        HWND box = GetDlgItem(hwnd, 10 + g);
        SetWindowPos(box, nullptr, margin, y, boxW, boxH, SWP_NOZORDER | SWP_NOACTIVATE);
        int x = margin + S(12);
        for (int i : kSettingsOrder) {
            if (kItems[i].group != g) continue;
            SetWindowPos(GetDlgItem(hwnd, ID_ITEM_BASE + i), nullptr, x, y + S(22), checkW, checkH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            x += checkW;
        }
        y += boxH + gap;
    }
    HWND hint = GetDlgItem(hwnd, 20);
    SetWindowPos(hint, nullptr, margin, y, boxW, S(20), SWP_NOZORDER | SWP_NOACTIVATE);
    y += S(20) + margin;

    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_settingsFont), TRUE);

    RECT rc = {0, 0, boxW + 2 * margin, y};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(hwnd, GWL_STYLE), FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE), dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            for (int g = 0; g < kNumGroups; ++g)
                CreateWindowExW(0, L"BUTTON", GroupName(g), WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd,
                                reinterpret_cast<HMENU>(INT_PTR(10 + g)), cs->hInstance, nullptr);
            for (int i : kSettingsOrder) {
                HWND cb = CreateWindowExW(0, L"BUTTON", T(kItems[i].name), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                          0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(INT_PTR(ID_ITEM_BASE + i)),
                                          cs->hInstance, nullptr);
                SendMessageW(cb, BM_SETCHECK, (g_items & (1u << i)) ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            CreateWindowExW(0, L"STATIC", T(Str::SetHint),
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(INT_PTR(20)), cs->hInstance,
                            nullptr);
            LayoutSettings(hwnd, GetDpiForWindow(hwnd));
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (HIWORD(wp) == BN_CLICKED && id >= ID_ITEM_BASE && id < ID_ITEM_BASE + IT_COUNT) {
                const DWORD bit = 1u << (id - ID_ITEM_BASE);
                const bool on = SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED;
                const DWORD next = on ? (g_items | bit) : (g_items & ~bit);
                if (!next) {  // keep at least one item visible
                    SendMessageW(reinterpret_cast<HWND>(lp), BM_SETCHECK, BST_CHECKED, 0);
                    return 0;
                }
                g_items = next;
                SaveSettings();
                Render();
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutSettings(hwnd, HIWORD(wp));
            return 0;
        }
        case WM_DESTROY:
            g_settings = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void ShowSettings() {
    if (g_settings) {
        SetForegroundWindow(g_settings);
        return;
    }
    g_settings = CreateWindowExW(0, kSettingsClass, T(Str::SetTitle),
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, 0, 100, 100,
                                 nullptr, nullptr, g_inst, nullptr);
    if (!g_settings) return;
    // Center on the monitor with the taskbar's work area.
    RECT wr, rc;
    GetWindowRect(g_settings, &rc);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wr, 0);
    SetWindowPos(g_settings, nullptr, wr.left + (wr.right - wr.left - (rc.right - rc.left)) / 2,
                 wr.top + (wr.bottom - wr.top - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    ShowWindow(g_settings, SW_SHOWNORMAL);
    SetForegroundWindow(g_settings);
}

namespace {

// ------------------------------------------------------------------ attach to taskbar

void Attach() {
    HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tb) return;
    if (g_strip && IsWindow(g_strip) && GetParent(g_strip) == tb) return;
    if (g_strip && IsWindow(g_strip)) DestroyWindow(g_strip);
    g_strip = nullptr;
    g_taskbar = tb;
    g_strip = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kStripClass, kAppName,
                              WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, tb, nullptr, g_inst, nullptr);
    if (!g_strip) {
        // Fallback: create unparented, then reparent into the taskbar.
        g_strip = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kStripClass, kAppName,
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
        if (g_strip) {
            SetWindowLongPtrW(g_strip, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
            SetParent(g_strip, tb);
        }
    }
}

// ------------------------------------------------------------------ menu
// The UI thread shares explorer's input queue, so anything that can block
// (spawning schtasks, UAC prompts, launching programs) runs on a short-lived
// background thread instead.

LONG g_autostart = -1;  // -1 unknown, 0/1; refreshed off the UI thread

void RunAsync(LPTHREAD_START_ROUTINE fn) {
    HANDLE t = CreateThread(nullptr, 0, fn, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

DWORD WINAPI RefreshAutostart(void*) {
    InterlockedExchange(&g_autostart, IsAutostart() ? 1 : 0);
    return 0;
}

DWORD WINAPI ToggleAutostart(void*) {
    SetAutostart(InterlockedCompareExchange(&g_autostart, 0, 0) != 1);
    return RefreshAutostart(nullptr);
}

DWORD WINAPI OpenTaskManagerThread(void*) {
    ShellExecuteW(nullptr, nullptr, L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

DWORD WINAPI OpenPawnIoSite(void*) {
    ShellExecuteW(nullptr, nullptr, L"https://pawnio.eu/", nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}

DWORD WINAPI RelaunchElevated(void*) {
    wchar_t exe[MAX_PATH];
    ExePath(exe);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"/restart";
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) PostMessageW(g_host, WM_CLOSE, 0, 0);  // new instance waits for our mutex
    return 0;
}

RECT StripScreenRect(int left = -1, int right = -1) {
    RECT r;
    GetWindowRect(g_strip, &r);
    if (left >= 0) {
        r.right = r.left + right;
        r.left += left;
    }
    return r;
}

RECT PanelAnchor(bool fromTray);
void UpdateTray(bool add);

void ApplyLanguage(Lang lang) {
    SetLang(lang);
    SaveSettings();
    // Windows that baked in text: rebuild them in the new language.
    panel::Close();
    if (g_settings) {
        DestroyWindow(g_settings);
        ShowSettings();
    }
    UpdateTray(false);
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_DETAILS, T(Str::MenuDetails));
    AppendMenuW(menu, MF_STRING, ID_TASKMGR, T(Str::MenuTaskMgr));
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, T(Str::MenuSettings));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    switch (g_hwState) {
        case HwSensorState::NotInstalled: AppendMenuW(menu, MF_STRING, ID_PAWNIO, T(Str::MenuPawnIo)); break;
        case HwSensorState::NeedsAdmin: AppendMenuW(menu, MF_STRING, ID_ELEVATE, T(Str::MenuElevate)); break;
        case HwSensorState::Unsupported: AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, T(Str::MenuUnsupported)); break;
        case HwSensorState::Ok: break;
    }
    const LONG autostart = InterlockedCompareExchange(&g_autostart, 0, 0);
    AppendMenuW(menu, MF_STRING | (autostart == 1 ? MF_CHECKED : 0) | (autostart < 0 ? MF_GRAYED : 0), ID_AUTOSTART,
                T(Str::MenuAutostart));

    // Language names are shown in their own language so either is findable.
    HMENU langMenu = CreatePopupMenu();
    const Lang pref = GetLangPref();
    AppendMenuW(langMenu, MF_STRING, ID_LANG_AUTO, T(Str::MenuLangAuto));
    AppendMenuW(langMenu, MF_STRING, ID_LANG_ZH, L"简体中文");  // 简体中文
    AppendMenuW(langMenu, MF_STRING, ID_LANG_EN, L"English");
    CheckMenuRadioItem(langMenu, ID_LANG_AUTO, ID_LANG_EN, ID_LANG_AUTO + int(pref), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(langMenu), T(Str::MenuLanguage));  // owned by menu now

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, T(Str::MenuExit));

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_host);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_host, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_host, WM_NULL, 0, 0);

    switch (cmd) {
        case ID_DETAILS: panel::Toggle(panel::Mode::Overview, PanelAnchor(false)); break;
        case ID_LANG_AUTO:
        case ID_LANG_ZH:
        case ID_LANG_EN: ApplyLanguage(Lang(cmd - ID_LANG_AUTO)); break;
        case ID_TASKMGR: RunAsync(OpenTaskManagerThread); break;
        case ID_SETTINGS: ShowSettings(); break;
        case ID_PAWNIO: RunAsync(OpenPawnIoSite); break;
        case ID_ELEVATE: RunAsync(RelaunchElevated); break;
        case ID_AUTOSTART: RunAsync(ToggleAutostart); break;
        case ID_EXIT: DestroyWindow(g_host); break;
    }
}

// Left click: the network group opens the per-process view, anything else the overview.
void OnStripClick(int x) {
    constexpr int kNetGroup = 3;
    if (g_lay.groupL[kNetGroup] >= 0 && x >= g_lay.groupL[kNetGroup] - g_lay.colGap / 2)
        panel::Toggle(panel::Mode::Network, StripScreenRect(g_lay.groupL[kNetGroup], g_lay.groupR[kNetGroup]));
    else
        panel::Toggle(panel::Mode::Overview, StripScreenRect());
}

// ------------------------------------------------------------------ notification-area icon
// Always reachable, even while the strip is collapsed away or if something
// goes wrong with it: left click opens the panel, right click the menu.

UINT g_trayDpi;
bool g_trayAdded, g_trayHiddenTip;

void UpdateTray(bool add) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    const UINT dpi = g_taskbar ? GetDpiForWindow(g_taskbar) : 96;
    nid.hIcon = AppIcon(GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96));
    if (g_stripHidden)
        swprintf(nid.szTip, ARRAYSIZE(nid.szTip), L"%ls\n%ls", T(Str::AppName), T(Str::TrayHidden));
    else
        wcsncpy(nid.szTip, T(Str::AppName), ARRAYSIZE(nid.szTip) - 1);
    if (add || !g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &nid);  // after an explorer restart the old entry is gone anyway
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        nid.uVersion = NOTIFYICON_VERSION_4;
        if (g_trayAdded) Shell_NotifyIconW(NIM_SETVERSION, &nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    g_trayDpi = dpi;
    g_trayHiddenTip = g_stripHidden;
}

void RemoveTray() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// Where the panel should open: above the strip, or above the tray icon when
// the strip is collapsed (or the request came from the icon).
RECT PanelAnchor(bool fromTray) {
    if (!fromTray && g_strip && IsWindowVisible(g_strip)) return StripScreenRect();
    NOTIFYICONIDENTIFIER id = {sizeof(id)};
    id.hWnd = g_host;
    id.uID = kTrayId;
    RECT r;
    if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &r))) return r;
    POINT pt;
    GetCursorPos(&pt);
    return {pt.x, pt.y, pt.x + 1, pt.y + 1};
}

// ------------------------------------------------------------------ window procs

LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP: OnStripClick(short(LOWORD(lp))); return 0;
        case WM_RBUTTONUP: ShowMenu(); return 0;
        case WM_DPICHANGED_AFTERPARENT: Render(); return 0;
        case WM_DESTROY:
            // Also happens when explorer exits and takes its child windows along.
            if (hwnd == g_strip) g_strip = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgQuit && msg) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == g_msgShow && msg) {  // "/show [network]" from another instance, e.g. bound to a hotkey
        panel::Toggle(wp ? panel::Mode::Network : panel::Mode::Overview, PanelAnchor(false));
        return 0;
    }
    if (msg == g_msgLang && msg) {  // "/lang auto|zh|en" from another instance
        if (wp <= WPARAM(Lang::English)) ApplyLanguage(Lang(wp));
        return 0;
    }
    if (msg == g_msgShellHook && msg) {
        switch (wp & 0x7FFF) {
            case HSHELL_WINDOWCREATED:
            case HSHELL_WINDOWDESTROYED:
            case HSHELL_WINDOWREPLACED: MarkTaskbarDirty(); break;
        }
        return 0;
    }
    if (msg == g_msgTaskbarCreated && msg) {
        MarkTaskbarDirty();
        Attach();
        Render();
        UpdateTray(true);
        return 0;
    }
    switch (msg) {
        case WM_APP_METRICS: {
            Attach();  // no-op unless explorer restarted without us noticing
            Render();
            panel::OnData();
            const UINT dpi = g_taskbar ? GetDpiForWindow(g_taskbar) : 96;
            if (!g_trayAdded || dpi != g_trayDpi || g_stripHidden != g_trayHiddenTip) UpdateTray(false);
            return 0;
        }
        case WM_APP_TRAY:  // NOTIFYICON_VERSION_4: event in LOWORD(lParam)
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT: panel::Toggle(panel::Mode::Overview, PanelAnchor(true)); break;
                case WM_CONTEXTMENU: ShowMenu(); break;
            }
            return 0;
        case WM_SETTINGCHANGE:
            MarkTaskbarDirty();  // e.g. alignment or widgets toggled ("TraySettings")
            if (lp && lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                g_light = ReadLightTheme();
                Render();
                panel::OnThemeChanged();
            }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED: panel::OnThemeChanged(); return 0;
        case WM_DISPLAYCHANGE: MarkTaskbarDirty(); Render(); return 0;
        case WM_DESTROY:
            DeregisterShellHookWindow(hwnd);
            RemoveTray();
            panel::Close();
            if (g_settings) DestroyWindow(g_settings);
            if (g_strip) DestroyWindow(g_strip);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ worker

// Where the taskbar's own items end, so the strip can make room for them.
int QueryOccupiedRight() {
    HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND tray = tb ? FindWindowExW(tb, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    if (!tray) return -1;
    RECT r;
    GetWindowRect(tray, &r);
    return tbspace::OccupiedRight(tb, r.left);
}

// Starts/stops the on-demand collectors to match what the panel shows.
void ApplyWants() {
    g_collector.SetPerCore(InterlockedCompareExchange(&g_wantPerCore, 0, 0) != 0);
    const bool wantNet = InterlockedCompareExchange(&g_wantNetProc, 0, 0) != 0;
    const netproc::State st = netproc::GetState();
    if (wantNet && st != netproc::State::Running) {
        const netproc::State now = netproc::Start();
        AcquireSRWLockExclusive(&g_lock);
        g_shared.netState = now;
        g_shared.topCount = 0;
        g_shared.topValid = false;
        ReleaseSRWLockExclusive(&g_lock);
        PostMessageW(g_host, WM_APP_METRICS, 0, 0);
    } else if (!wantNet && st == netproc::State::Running) {
        netproc::Stop();
        AcquireSRWLockExclusive(&g_lock);
        g_shared.netState = netproc::State::Off;
        g_shared.topCount = 0;
        g_shared.topValid = false;
        ReleaseSRWLockExclusive(&g_lock);
    }
}

DWORD WINAPI Worker(void*) {
    // UI Automation (taskbar layout) needs COM; MTA keeps its cross-process
    // calls off any message loop.
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    g_collector.Init();
    g_hwState = g_collector.HwState();
    RefreshAutostart(nullptr);
    static netproc::Entry top[kTopProcesses];
    HANDLE waits[] = {g_stopEvent, g_wakeEvent};
    ULONGLONG next = GetTickCount64();
    for (int tick = 0;; ++tick) {
        // NVML and PDH touch a lot of memory while initializing that is never
        // used again; hand it back once the steady state has been reached.
        if (tick == 2) SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
        next += kIntervalMs;
        ULONGLONG now = GetTickCount64();
        if (now > next + kIntervalMs) next = now;  // resumed from sleep
        bool stop = false;
        for (;;) {  // a wake only re-applies what the UI wants; sampling keeps its cadence
            now = GetTickCount64();
            const DWORD r = WaitForMultipleObjects(2, waits, FALSE, next > now ? DWORD(next - now) : 0);
            if (r == WAIT_OBJECT_0) stop = true;
            if (r != WAIT_OBJECT_0 + 1) break;
            ApplyWants();
        }
        if (stop) break;

        ApplyWants();
        Metrics m;
        g_collector.Sample(m);
        // Event-driven (see MarkTaskbarDirty), plus a slow safety refresh.
        int occupied = -2;  // -2: keep the previous value
        if (InterlockedCompareExchange(&g_taskbarDirty, 0, 0) > 0) {
            InterlockedDecrement(&g_taskbarDirty);
            occupied = QueryOccupiedRight();
        } else if (tick % 30 == 0) {
            occupied = QueryOccupiedRight();
        }
        const bool netRunning = netproc::GetState() == netproc::State::Running;
        const int topCount = netRunning ? netproc::Collect(top, kTopProcesses) : 0;

        AcquireSRWLockExclusive(&g_lock);
        Shared& s = g_shared;
        s.m = m;
        s.have = true;
        s.hist[H_CPU].Push(float(m.cpuUtil));
        s.hist[H_GPU].Push(float(m.gpuUtil));
        s.hist[H_MEM].Push(float(m.memUtil));
        s.hist[H_DISK_R].Push(float(m.diskRead));
        s.hist[H_DISK_W].Push(float(m.diskWrite));
        s.hist[H_NET_DOWN].Push(float(m.netDown));
        s.hist[H_NET_UP].Push(float(m.netUp));
        if (occupied != -2) s.occupiedRight = occupied;
        if (netRunning) {
            for (int i = 0; i < topCount; ++i) s.top[i] = top[i];
            s.topCount = topCount;
            s.topValid = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
        PostMessageW(g_host, WM_APP_METRICS, 0, 0);
    }
    netproc::Stop();
    tbspace::Shutdown();
    g_collector.Shutdown();
    if (com) CoUninitialize();
    return 0;
}

}  // namespace

void OpenTaskManager() { RunAsync(OpenTaskManagerThread); }

namespace {
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    g_inst = inst;

    // Single instance. "/restart" (self-relaunch) waits for the previous
    // instance to exit; "/replace" (used when updating) also asks it to quit.
    g_msgQuit = RegisterWindowMessageW(L"TaskbarMonitor.Quit");
    g_msgShow = RegisterWindowMessageW(L"TaskbarMonitor.Show");
    if (const wchar_t* show = wcsstr(cmdLine, L"/show")) {  // toggle the running instance's panel
        if (HWND running = FindWindowW(kHostClass, nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);  // let it activate the panel
            PostMessageW(running, g_msgShow, wcsstr(show, L"network") ? 1 : 0, 0);
        }
        return 0;
    }
    g_msgLang = RegisterWindowMessageW(L"TaskbarMonitor.Language");
    if (const wchar_t* lang = wcsstr(cmdLine, L"/lang")) {  // switch the running instance's language
        const Lang l = wcsstr(lang, L"en") ? Lang::English : wcsstr(lang, L"zh") ? Lang::Chinese : Lang::Auto;
        if (HWND running = FindWindowW(kHostClass, nullptr)) PostMessageW(running, g_msgLang, WPARAM(l), 0);
        return 0;
    }
    const bool replace = wcsstr(cmdLine, L"/replace") != nullptr;
    const bool takeOver = replace || wcsstr(cmdLine, L"/restart") != nullptr;
    if (replace)
        if (HWND old = FindWindowW(kHostClass, nullptr)) PostMessageW(old, g_msgQuit, 0, 0);
    const ULONGLONG deadline = GetTickCount64() + (takeOver ? 10000 : 0);
    for (;;) {
        // Fails with access denied while an elevated instance holds it.
        g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\TaskbarMonitor.SingleInstance");
        if (g_mutex) {
            const ULONGLONG now = GetTickCount64();
            DWORD wait = WaitForSingleObject(g_mutex, deadline > now ? DWORD(deadline - now) : 0);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return 0;
            break;
        }
        if (GetTickCount64() >= deadline) return 0;
        Sleep(100);
    }

    // Background utility: ask the scheduler for efficiency mode (EcoQoS).
    PROCESS_POWER_THROTTLING_STATE pt = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                         PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
                                         PROCESS_POWER_THROTTLING_EXECUTION_SPEED};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));

    LoadSettings();
    g_light = ReadLightTheme();
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = StripProc;
    wc.lpszClassName = kStripClass;
    wc.style = 0;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = kSettingsClass;
    wc.style = 0;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // taskbar button / Alt+Tab
    wc.hIconSm = AppIcon(GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&wc);

    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                             nullptr);
    if (!g_host) return 1;
    // When elevated, UIPI would drop these broadcasts from explorer.
    ChangeWindowMessageFilterEx(g_host, g_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_host, WM_SETTINGCHANGE, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_host, g_msgQuit, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_host, g_msgShow, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_host, g_msgLang, MSGFLT_ALLOW, nullptr);
    g_msgShellHook = RegisterWindowMessageW(L"SHELLHOOK");
    ChangeWindowMessageFilterEx(g_host, g_msgShellHook, MSGFLT_ALLOW, nullptr);
    RegisterShellHookWindow(g_host);
    ChangeWindowMessageFilterEx(g_host, WM_APP_TRAY, MSGFLT_ALLOW, nullptr);

    Attach();
    Render();
    UpdateTray(true);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);  // NVML needs a full-size stack

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_settings && IsDialogMessageW(g_settings, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_stopEvent);
    WaitForSingleObject(g_worker, 5000);
    ReleaseMutex(g_mutex);
    return 0;
}
