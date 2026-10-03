#include "metrics.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <intrin.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "pawnio.h"

namespace {

// ---------------------------------------------------------------- CPU (PDH)
// Same counters Task Manager uses: "% Processor Utility" for load and
// "% Processor Performance" x "Processor Frequency" for the current speed.
PDH_HQUERY g_pdh = nullptr;
PDH_HCOUNTER g_cUtil = nullptr, g_cPerf = nullptr, g_cFreq = nullptr;
PDH_HCOUNTER g_cDiskRead = nullptr, g_cDiskWrite = nullptr;
PDH_HCOUNTER g_cCores = nullptr;  // wildcard instance array, only while requested
bool g_coresFresh = false;        // rate counters need one collection before valid
alignas(8) BYTE g_coreBuf[32 * 1024];

// Fallback when PDH is unavailable.
ULONGLONG g_lastIdle = 0, g_lastTotal = 0;

ULONGLONG FtToU64(const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

bool PdhValue(PDH_HCOUNTER c, double& out) {
    if (!c) return false;
    PDH_FMT_COUNTERVALUE v;
    if (PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS) return false;
    if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) return false;
    out = v.doubleValue;
    return true;
}

// Instances are named "<group>,<number>" plus "_Total"/"<group>,_Total";
// order logical processors by (group, number).
void SampleCores(Metrics& m) {
    DWORD size = sizeof(g_coreBuf), count = 0;
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(g_coreBuf);
    if (PdhGetFormattedCounterArrayW(g_cCores, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS)
        return;
    int keys[kMaxCores];
    int n = 0;
    for (DWORD i = 0; i < count && n < kMaxCores; ++i) {
        const wchar_t* name = items[i].szName;
        if (wcschr(name, L'_')) continue;  // totals
        const wchar_t* comma = wcschr(name, L',');
        if (!comma) continue;
        int key = _wtoi(name) * 4096 + _wtoi(comma + 1);
        double v = items[i].FmtValue.doubleValue;
        float util = float(v < 0 ? 0 : (v > 100 ? 100 : v));
        int j = n++;  // insertion sort by key
        while (j > 0 && keys[j - 1] > key) {
            keys[j] = keys[j - 1];
            m.coreUtil[j] = m.coreUtil[j - 1];
            --j;
        }
        keys[j] = key;
        m.coreUtil[j] = util;
    }
    m.coreCount = n;
}

// ---------------------------------------------------------------- Network
// Sum over physical adapters only: filter drivers and virtual switches would
// otherwise count the same traffic several times. Deltas are matched per
// interface LUID so adapters appearing/disappearing don't cause spikes.
struct IfCounter { ULONG64 luid; ULONG64 in, out; };
constexpr int kMaxIf = 64;
IfCounter g_ifPrev[kMaxIf];
int g_ifPrevCount = -1;  // -1: no previous sample

bool IsPhysical(const MIB_IF_ROW2& r) {
    return r.InterfaceAndOperStatusFlags.HardwareInterface &&
           !r.InterfaceAndOperStatusFlags.FilterInterface &&
           r.Type != IF_TYPE_SOFTWARE_LOOPBACK &&
           r.OperStatus == IfOperStatusUp;
}

// ---------------------------------------------------------------- GPU (NVML)
typedef int nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
struct nvmlUtilization_t { unsigned int gpu, memory; };
struct nvmlMemory_t { unsigned long long total, free, used; };
typedef nvmlReturn_t (*PFN_nvmlInit)();
typedef nvmlReturn_t (*PFN_nvmlShutdown)();
typedef nvmlReturn_t (*PFN_nvmlGetCount)(unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlGetHandle)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*PFN_nvmlGetUtil)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (*PFN_nvmlGetTemp)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlGetPower)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlGetClock)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlGetMemory)(nvmlDevice_t, nvmlMemory_t*);
typedef nvmlReturn_t (*PFN_nvmlGetName)(nvmlDevice_t, char*, unsigned int);
constexpr int NVML_TEMPERATURE_GPU = 0;
constexpr int NVML_CLOCK_GRAPHICS = 0;

HMODULE g_nvml = nullptr;
nvmlDevice_t g_gpu = nullptr;
PFN_nvmlShutdown g_nvmlShutdown;
PFN_nvmlGetUtil g_nvmlUtil;
PFN_nvmlGetTemp g_nvmlTemp;
PFN_nvmlGetPower g_nvmlPower;
PFN_nvmlGetClock g_nvmlClock;
PFN_nvmlGetMemory g_nvmlMemory;

bool InitNvml(wchar_t* name, int nameLen) {
    g_nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_nvml) {
        wchar_t path[MAX_PATH];
        if (ExpandEnvironmentStringsW(L"%ProgramFiles%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, MAX_PATH))
            g_nvml = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!g_nvml) return false;
    auto init = (PFN_nvmlInit)GetProcAddress(g_nvml, "nvmlInit_v2");
    auto count = (PFN_nvmlGetCount)GetProcAddress(g_nvml, "nvmlDeviceGetCount_v2");
    auto handle = (PFN_nvmlGetHandle)GetProcAddress(g_nvml, "nvmlDeviceGetHandleByIndex_v2");
    g_nvmlShutdown = (PFN_nvmlShutdown)GetProcAddress(g_nvml, "nvmlShutdown");
    g_nvmlUtil = (PFN_nvmlGetUtil)GetProcAddress(g_nvml, "nvmlDeviceGetUtilizationRates");
    g_nvmlTemp = (PFN_nvmlGetTemp)GetProcAddress(g_nvml, "nvmlDeviceGetTemperature");
    g_nvmlPower = (PFN_nvmlGetPower)GetProcAddress(g_nvml, "nvmlDeviceGetPowerUsage");
    g_nvmlClock = (PFN_nvmlGetClock)GetProcAddress(g_nvml, "nvmlDeviceGetClockInfo");
    g_nvmlMemory = (PFN_nvmlGetMemory)GetProcAddress(g_nvml, "nvmlDeviceGetMemoryInfo");
    auto getName = (PFN_nvmlGetName)GetProcAddress(g_nvml, "nvmlDeviceGetName");
    unsigned int n = 0;
    if (!init || !count || !handle || !g_nvmlShutdown || init() != 0) goto fail;
    if (count(&n) != 0 || n == 0 || handle(0, &g_gpu) != 0) { g_nvmlShutdown(); goto fail; }
    {
        char utf8[96];
        if (getName && getName(g_gpu, utf8, sizeof(utf8)) == 0)
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, name, nameLen);
    }
    return true;
fail:
    FreeLibrary(g_nvml);
    g_nvml = nullptr;
    g_gpu = nullptr;
    return false;
}

// ---------------------------------------------------------------- CPU temp/power (PawnIO)
// Intel: package temperature from IA32_PACKAGE_THERM_STATUS relative to TjMax,
// package power from the RAPL energy counter. Both MSRs are package-scoped, so
// the CPU the read happens to run on doesn't matter.
constexpr ULONG64 MSR_RAPL_POWER_UNIT = 0x606;
constexpr ULONG64 MSR_PKG_ENERGY_STATUS = 0x611;
constexpr ULONG64 MSR_IA32_TEMPERATURE_TARGET = 0x1A2;
constexpr ULONG64 MSR_IA32_PACKAGE_THERM_STATUS = 0x1B1;

PawnIo g_pawn;
double g_energyUnit = 0;  // joules per RAPL count
int g_tjMax = 100;
DWORD g_lastEnergy = 0;
bool g_haveEnergy = false;

bool ReadMsr(ULONG64 msr, ULONG64& v) { return g_pawn.Execute("ioctl_read_msr", &msr, 1, &v, 1); }

bool IsIntelCpu() {
    int r[4];
    __cpuid(r, 0);
    char vendor[13];
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    return strcmp(vendor, "GenuineIntel") == 0;
}

HwSensorState InitCpuHw() {
    if (!IsIntelCpu()) return HwSensorState::Unsupported;
    HRSRC res = FindResourceW(nullptr, L"INTELMSR", MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!res) return HwSensorState::Unsupported;
    const void* blob = LockResource(LoadResource(nullptr, res));
    DWORD size = SizeofResource(nullptr, res);
    switch (g_pawn.Open(blob, size)) {
        case PawnIo::Result::Ok: break;
        case PawnIo::Result::NotInstalled: return HwSensorState::NotInstalled;
        case PawnIo::Result::AccessDenied: return HwSensorState::NeedsAdmin;
        default: return HwSensorState::Unsupported;
    }
    ULONG64 v;
    if (ReadMsr(MSR_IA32_TEMPERATURE_TARGET, v)) {
        int tj = int((v >> 16) & 0xFF);
        if (tj > 0) g_tjMax = tj;
    }
    if (ReadMsr(MSR_RAPL_POWER_UNIT, v)) g_energyUnit = 1.0 / double(1ull << ((v >> 8) & 0x1F));
    return HwSensorState::Ok;
}

}  // namespace

bool Collector::Init() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    qpcFreq_ = f.QuadPart;
    lastQpc_ = c.QuadPart;

    if (PdhOpenQueryW(nullptr, 0, &g_pdh) == ERROR_SUCCESS) {
        PdhAddEnglishCounterW(g_pdh, L"\\Processor Information(_Total)\\% Processor Utility", 0, &g_cUtil);
        PdhAddEnglishCounterW(g_pdh, L"\\Processor Information(_Total)\\% Processor Performance", 0, &g_cPerf);
        PdhAddEnglishCounterW(g_pdh, L"\\Processor Information(_Total)\\Processor Frequency", 0, &g_cFreq);
        PdhAddEnglishCounterW(g_pdh, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &g_cDiskRead);
        PdhAddEnglishCounterW(g_pdh, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &g_cDiskWrite);
        PdhCollectQueryData(g_pdh);
    }
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        g_lastIdle = FtToU64(idle);
        g_lastTotal = FtToU64(kernel) + FtToU64(user);
    }

    DWORD size = sizeof(cpuName_);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                 RRF_RT_REG_SZ, nullptr, cpuName_, &size);

    InitNvml(gpuName_, sizeof(gpuName_) / sizeof(gpuName_[0]));
    hwState_ = InitCpuHw();

    Metrics dummy;
    SampleNetwork(dummy, 0);  // establish the baseline
    if (hwState_ == HwSensorState::Ok) SampleCpuHw(dummy, 0);
    return true;
}

void Collector::Sample(Metrics& m) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - lastQpc_) / double(qpcFreq_);
    lastQpc_ = now.QuadPart;

    SampleCpu(m);
    SampleMemory(m);
    SampleNetwork(m, dt);
    SampleGpu(m);
    if (hwState_ == HwSensorState::Ok) SampleCpuHw(m, dt);
}

void Collector::SampleCpu(Metrics& m) {
    bool havePdhUtil = false;
    if (g_pdh && PdhCollectQueryData(g_pdh) == ERROR_SUCCESS) {
        double util, perf, freq;
        if (PdhValue(g_cUtil, util)) {
            m.cpuUtil = util < 0 ? 0 : (util > 100 ? 100 : util);
            havePdhUtil = true;
        }
        if (PdhValue(g_cPerf, perf) && PdhValue(g_cFreq, freq)) m.cpuMHz = freq * perf / 100.0;
        double rd, wr;
        if (PdhValue(g_cDiskRead, rd)) m.diskRead = rd;
        if (PdhValue(g_cDiskWrite, wr)) m.diskWrite = wr;
        if (g_cCores && !g_coresFresh) SampleCores(m);
        g_coresFresh = false;
    }
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        ULONGLONG i = FtToU64(idle), t = FtToU64(kernel) + FtToU64(user);
        if (!havePdhUtil && t > g_lastTotal)
            m.cpuUtil = 100.0 * double((t - g_lastTotal) - (i - g_lastIdle)) / double(t - g_lastTotal);
        g_lastIdle = i;
        g_lastTotal = t;
    }
}

void Collector::SampleMemory(Metrics& m) {
    MEMORYSTATUSEX ms = {sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys) {
        ULONGLONG used = ms.ullTotalPhys - ms.ullAvailPhys;
        m.memUtil = 100.0 * double(used) / double(ms.ullTotalPhys);
        m.memUsedGB = double(used) / (1024.0 * 1024.0 * 1024.0);
        m.memTotalGB = double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
        // ullTotalPageFile is the commit limit (RAM + page files).
        m.commitLimitGB = double(ms.ullTotalPageFile) / (1024.0 * 1024.0 * 1024.0);
        m.commitUsedGB = double(ms.ullTotalPageFile - ms.ullAvailPageFile) / (1024.0 * 1024.0 * 1024.0);
    }
}

void Collector::SampleNetwork(Metrics& m, double dt) {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) return;
    IfCounter cur[kMaxIf];
    int n = 0;
    for (ULONG i = 0; i < table->NumEntries && n < kMaxIf; ++i) {
        const MIB_IF_ROW2& r = table->Table[i];
        if (!IsPhysical(r)) continue;
        cur[n++] = {r.InterfaceLuid.Value, r.InOctets, r.OutOctets};
    }
    FreeMibTable(table);

    if (g_ifPrevCount >= 0 && dt > 0) {
        ULONG64 din = 0, dout = 0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < g_ifPrevCount; ++j)
                if (g_ifPrev[j].luid == cur[i].luid) {
                    if (cur[i].in >= g_ifPrev[j].in) din += cur[i].in - g_ifPrev[j].in;
                    if (cur[i].out >= g_ifPrev[j].out) dout += cur[i].out - g_ifPrev[j].out;
                    break;
                }
        m.netDown = double(din) / dt;
        m.netUp = double(dout) / dt;
    }
    memcpy(g_ifPrev, cur, sizeof(IfCounter) * n);
    g_ifPrevCount = n;
}

void Collector::SampleGpu(Metrics& m) {
    if (!g_gpu) return;
    nvmlUtilization_t u;
    unsigned int v;
    if (g_nvmlUtil && g_nvmlUtil(g_gpu, &u) == 0) m.gpuUtil = u.gpu;
    if (g_nvmlClock && g_nvmlClock(g_gpu, NVML_CLOCK_GRAPHICS, &v) == 0) m.gpuMHz = v;
    if (g_nvmlTemp && g_nvmlTemp(g_gpu, NVML_TEMPERATURE_GPU, &v) == 0) m.gpuTemp = v;
    if (g_nvmlPower && g_nvmlPower(g_gpu, &v) == 0) m.gpuPower = v / 1000.0;
    nvmlMemory_t mem;
    if (g_nvmlMemory && g_nvmlMemory(g_gpu, &mem) == 0) {
        m.gpuMemUsedGB = double(mem.used) / (1024.0 * 1024.0 * 1024.0);
        m.gpuMemTotalGB = double(mem.total) / (1024.0 * 1024.0 * 1024.0);
    }
}

void Collector::SampleCpuHw(Metrics& m, double dt) {
    ULONG64 v;
    if (ReadMsr(MSR_IA32_PACKAGE_THERM_STATUS, v)) m.cpuTemp = g_tjMax - int((v >> 16) & 0x7F);
    if (g_energyUnit > 0 && ReadMsr(MSR_PKG_ENERGY_STATUS, v)) {
        DWORD e = DWORD(v);  // 32-bit wrapping counter
        if (g_haveEnergy && dt > 0) m.cpuPower = double(DWORD(e - g_lastEnergy)) * g_energyUnit / dt;
        g_lastEnergy = e;
        g_haveEnergy = true;
    }
}

void Collector::SetPerCore(bool on) {
    if (!g_pdh || on == (g_cCores != nullptr)) return;
    if (on) {
        if (PdhAddEnglishCounterW(g_pdh, L"\\Processor Information(*)\\% Processor Utility", 0, &g_cCores) != ERROR_SUCCESS)
            g_cCores = nullptr;
        g_coresFresh = true;
    } else {
        PdhRemoveCounter(g_cCores);
        g_cCores = nullptr;
    }
}

void Collector::Shutdown() {
    if (g_pdh) PdhCloseQuery(g_pdh);
    g_pdh = nullptr;
    if (g_nvml) {
        g_nvmlShutdown();
        FreeLibrary(g_nvml);
        g_nvml = nullptr;
        g_gpu = nullptr;
    }
    g_pawn.Close();
}
