#include "netproc.h"

#include <evntrace.h>
#include <evntcons.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

namespace netproc {
namespace {

// Microsoft-Windows-Kernel-Network
const GUID kKernelNetwork = {0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
constexpr wchar_t kSessionName[] = L"TaskbarMonitor-Net";

// Data events: TCP send/recv (IPv4 10/11, IPv6 26/27), UDP send/recv
// (IPv4 42/43, IPv6 58/59). Every one of them starts with
// PID:uint32, size:uint32, daddr, saddr.
constexpr USHORT kEventIds[] = {10, 11, 26, 27, 42, 43, 58, 59};
bool IsSend(USHORT id) { return id == 10 || id == 26 || id == 42 || id == 58; }
bool IsRecv(USHORT id) { return id == 11 || id == 27 || id == 43 || id == 59; }
bool IsV6(USHORT id) { return id == 26 || id == 27 || id == 58 || id == 59; }

// EVENT_FILTER_TYPE_EVENT_ID (Windows 8.1+), not in every SDK's headers.
constexpr ULONG kFilterTypeEventId = 0x80000200;
struct EventIdFilter {
    BOOLEAN filterIn;
    UCHAR reserved;
    USHORT count;
    USHORT events[sizeof(kEventIds) / sizeof(kEventIds[0])];
};

constexpr TRACEHANDLE kInvalidTrace = ~TRACEHANDLE(0);  // INVALID_PROCESSTRACE_HANDLE

// Bytes per PID since the last Collect(), filled from the ETW callback thread.
struct Slot {
    DWORD pid;  // 0 = empty; PID 0 itself is stored as kPidIdle
    ULONG64 down, up;
};
constexpr DWORD kPidIdle = 0xFFFFFFFF;
constexpr int kSlots = 1024;  // power of two, open addressing
Slot g_table[kSlots];
int g_used = 0;
SRWLOCK g_tableLock = SRWLOCK_INIT;

TRACEHANDLE g_session = 0, g_trace = kInvalidTrace;
HANDLE g_thread = nullptr;
State g_state = State::Off;
ULONGLONG g_lastCollect = 0;

// Image path cache for PIDs seen while running; reset on Start().
struct PidName {
    DWORD pid;
    wchar_t path[MAX_PATH];
};
constexpr int kNameCache = 1024;  // > table capacity, so paths stay valid within a Collect()
PidName g_names[kNameCache];
int g_nameCount = 0, g_nameNext = 0;

struct Props {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[64];
};

void InitProps(Props& props) {
    memset(&props, 0, sizeof(props));
    props.p.Wnode.BufferSize = sizeof(props);
    props.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props.p.Wnode.ClientContext = 1;  // QPC timestamps
    // EVENT_TRACE_USE_MS_FLUSH_TIMER: FlushTimer in ms, so per-process figures
    // line up with the once-per-second adapter totals instead of lagging a tick.
    props.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | 0x00000010;
    props.p.BufferSize = 64;  // KB
    props.p.MinimumBuffers = 4;
    props.p.MaximumBuffers = 32;
    props.p.FlushTimer = 100;  // ms
    props.p.LoggerNameOffset = offsetof(Props, name);
}

void Add(DWORD pid, ULONG64 down, ULONG64 up) {
    if (pid == 0) pid = kPidIdle;
    AcquireSRWLockExclusive(&g_tableLock);
    for (unsigned h = (pid * 2654435761u) & (kSlots - 1), i = 0; i < kSlots; ++i, h = (h + 1) & (kSlots - 1)) {
        Slot& s = g_table[h];
        if (s.pid == pid) {
            s.down += down;
            s.up += up;
            break;
        }
        if (s.pid == 0) {
            if (g_used >= kSlots * 3 / 4) break;  // saturated: drop rather than degrade
            s = {pid, down, up};
            ++g_used;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_tableLock);
}

void WINAPI OnEvent(PEVENT_RECORD r) {
    const USHORT id = r->EventHeader.EventDescriptor.Id;
    const bool send = IsSend(id), recv = IsRecv(id);
    if (!send && !recv) return;
    const bool v6 = IsV6(id);
    if (r->UserDataLength < (v6 ? 40 : 16)) return;
    const BYTE* d = static_cast<const BYTE*>(r->UserData);
    DWORD pid, size;
    memcpy(&pid, d, 4);
    memcpy(&size, d + 4, 4);
    // Skip loopback so the per-process figures add up to the adapter totals.
    if (v6) {
        static const BYTE kLoop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        if (!memcmp(d + 8, kLoop6, 16) || !memcmp(d + 24, kLoop6, 16)) return;
    } else if (d[8] == 127 || d[12] == 127) {
        return;
    }
    Add(pid, recv ? size : 0, send ? size : 0);
}

DWORD WINAPI TraceThread(void*) {
    ProcessTrace(&g_trace, 1, nullptr, nullptr);  // returns when the session stops
    return 0;
}

const wchar_t* ImagePath(DWORD pid) {
    for (int i = 0; i < g_nameCount; ++i)
        if (g_names[i].pid == pid) return g_names[i].path;
    PidName& e = g_names[g_nameNext];
    g_nameNext = (g_nameNext + 1) % kNameCache;
    if (g_nameCount < kNameCache) ++g_nameCount;
    e.pid = pid;
    e.path[0] = 0;
    if (pid != kPidIdle && pid != 4) {
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            DWORD n = MAX_PATH;
            if (!QueryFullProcessImageNameW(h, 0, e.path, &n)) e.path[0] = 0;
            CloseHandle(h);
        }
    }
    return e.path;
}

void NameFromPath(DWORD pid, const wchar_t* path, wchar_t* name, size_t cap) {
    if (path[0]) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        wcsncpy(name, slash ? slash + 1 : path, cap - 1);
        name[cap - 1] = 0;
    } else if (pid == 4 || pid == kPidIdle) {
        wcscpy(name, L"System");
    } else {
        swprintf(name, cap, L"PID %lu", pid);  // exited, or protected
    }
}

}  // namespace

State GetState() { return g_state; }

State Start() {
    if (g_state == State::Running) return g_state;

    Props props;
    InitProps(props);
    ControlTraceW(0, kSessionName, &props.p, EVENT_TRACE_CONTROL_STOP);  // left over from a crash

    InitProps(props);
    ULONG st = StartTraceW(&g_session, kSessionName, &props.p);
    if (st != ERROR_SUCCESS) {
        g_session = 0;
        return g_state = (st == ERROR_ACCESS_DENIED ? State::NeedsAdmin : State::Failed);
    }

    // Let the kernel drop the provider's other events (connects, retransmits鈥?.
    EventIdFilter filter = {TRUE, 0, USHORT(sizeof(kEventIds) / sizeof(kEventIds[0])), {}};
    memcpy(filter.events, kEventIds, sizeof(kEventIds));
    EVENT_FILTER_DESCRIPTOR desc = {ULONGLONG(&filter), ULONG(sizeof(filter)), kFilterTypeEventId};
    ENABLE_TRACE_PARAMETERS params = {};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &desc;
    params.FilterDescCount = 1;
    st = EnableTraceEx2(g_session, &kKernelNetwork, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0, 0,
                        0, &params);
    if (st != ERROR_SUCCESS)  // older systems: filter in the callback instead
        st = EnableTraceEx2(g_session, &kKernelNetwork, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0,
                            0, 0, nullptr);
    if (st == ERROR_SUCCESS) {
        EVENT_TRACE_LOGFILEW lf = {};
        lf.LoggerName = const_cast<wchar_t*>(kSessionName);
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = OnEvent;
        g_trace = OpenTraceW(&lf);
        if (g_trace != kInvalidTrace) g_thread = CreateThread(nullptr, 0, TraceThread, nullptr, 0, nullptr);
    }
    if (!g_thread) {
        Stop();
        return g_state = State::Failed;
    }

    AcquireSRWLockExclusive(&g_tableLock);
    memset(g_table, 0, sizeof(g_table));
    g_used = 0;
    ReleaseSRWLockExclusive(&g_tableLock);
    g_nameCount = g_nameNext = 0;
    g_lastCollect = GetTickCount64();
    return g_state = State::Running;
}

void Stop() {
    if (g_session) {
        Props props;
        InitProps(props);
        ControlTraceW(g_session, nullptr, &props.p, EVENT_TRACE_CONTROL_STOP);
        g_session = 0;
    }
    if (g_trace != kInvalidTrace) {
        CloseTrace(g_trace);
        g_trace = kInvalidTrace;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_state == State::Running) g_state = State::Off;
}

int Collect(Entry* out, int max) {
    if (g_state != State::Running) return 0;

    static Slot snap[kSlots];
    AcquireSRWLockExclusive(&g_tableLock);
    memcpy(snap, g_table, sizeof(snap));
    memset(g_table, 0, sizeof(g_table));
    g_used = 0;
    ReleaseSRWLockExclusive(&g_tableLock);

    const ULONGLONG now = GetTickCount64();
    const double dt = (now - g_lastCollect) / 1000.0;
    g_lastCollect = now;
    if (dt <= 0) return 0;

    // Merge PIDs that run the same image (browsers, services hosts鈥?.
    struct Group {
        const wchar_t* path;
        DWORD pid;
        ULONG64 down, up;
        int processes;
    };
    static Group groups[kSlots];
    int ng = 0;
    for (const Slot& s : snap) {
        if (!s.pid) continue;
        const wchar_t* path = ImagePath(s.pid);
        int g = 0;
        if (path[0])
            for (; g < ng; ++g)
                if (groups[g].path[0] && !_wcsicmp(groups[g].path, path)) break;
        if (!path[0] || g == ng) groups[ng++] = {path, s.pid, 0, 0, 0};
        groups[g].down += s.down;
        groups[g].up += s.up;
        groups[g].processes++;
    }

    int n = 0;
    for (int i = 0; i < ng; ++i) {  // insertion into the top-N list
        const ULONG64 total = groups[i].down + groups[i].up;
        if (!total) continue;
        int j = n < max ? n++ : max;
        while (j > 0 && out[j - 1].down + out[j - 1].up < total / dt) {
            if (j < max) out[j] = out[j - 1];
            --j;
        }
        if (j >= max) continue;
        Entry& e = out[j];
        e.down = groups[i].down / dt;
        e.up = groups[i].up / dt;
        e.processes = groups[i].processes;
        wcscpy(e.path, groups[i].path);
        NameFromPath(groups[i].pid, groups[i].path, e.name, 64);
    }
    return n;
}

}  // namespace netproc
