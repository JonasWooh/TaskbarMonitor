#pragma once

// User-visible strings in Simplified Chinese and English. The language
// follows the Windows display language unless the user picks one.

enum class Lang { Auto = 0, Chinese = 1, English = 2 };  // persisted values

enum class Str {
    AppName, TrayHidden,
    MenuDetails, MenuTaskMgr, MenuSettings, MenuPawnIo, MenuElevate, MenuUnsupported, MenuAutostart,
    MenuLanguage, MenuLangAuto, MenuExit,
    SetTitle, SetHint,
    GroupMemory, GroupNetwork,
    ItemUtil, ItemTemp, ItemFreq, ItemPower, ItemMemUtil, ItemMemUsed, ItemUp, ItemDown,
    PanelTitle, PanelTaskMgr, PanelSettings,
    Freq, Temp, Power, Vram, Cores, NoGpu,
    Memory, InUse, Available, Committed,
    Disk, AllDisks, ReadFmt, WriteFmt, ReadPeak, WritePeak,
    Network, AllAdapters, DownPeak, UpPeak, ByProcess,
    Process, Download, Upload, NeedsAdmin, EtwFailed, Collecting, NoActivity,
    NetTitle, Back,
    WhyPawnIo, WhyAdmin, WhyUnsupported,
    Count
};

void SetLang(Lang pref);  // Auto resolves against the Windows display language
Lang GetLangPref();
const wchar_t* T(Str id);
