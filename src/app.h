#pragma once

// State shared between the sampling thread, the taskbar strip and the panel.

#include <windows.h>

#include "metrics.h"
#include "netproc.h"

constexpr int kHistory = 60;  // seconds of history kept for the panel's charts

struct History {
    float v[kHistory];
    int count = 0, head = 0;  // head = next write position
    void Push(float x) {
        v[head] = x < 0 ? 0 : x;
        head = (head + 1) % kHistory;
        if (count < kHistory) ++count;
    }
    // i = 0 is the oldest sample still kept.
    float At(int i) const { return v[(head - count + i + kHistory) % kHistory]; }
    float Max() const {
        float m = 0;
        for (int i = 0; i < count; ++i)
            if (At(i) > m) m = At(i);
        return m;
    }
};

enum HistId { H_CPU, H_GPU, H_MEM, H_DISK_R, H_DISK_W, H_NET_DOWN, H_NET_UP, H_COUNT };

constexpr int kTopProcesses = 10;

struct Shared {
    Metrics m;
    bool have = false;  // at least one sample taken
    History hist[H_COUNT];
    netproc::State netState = netproc::State::Off;
    netproc::Entry top[kTopProcesses];
    int topCount = 0;
    bool topValid = false;  // at least one interval measured since the session started
    int occupiedRight = -1;  // screen x where the taskbar's own items end, -1 unknown
};

// Guarded by g_lock.
extern SRWLOCK g_lock;
extern Shared g_shared;

extern HINSTANCE g_inst;
extern HWND g_host, g_taskbar;
extern Collector g_collector;
extern HwSensorState g_hwState;
extern bool g_light;  // taskbar/system light theme

// What the UI currently needs from the sampling thread.
extern volatile LONG g_wantPerCore, g_wantNetProc;
void WakeWorker();

bool IsElevated();
HICON AppIcon(int size);  // shared (LR_SHARED): never destroy
void OpenTaskManager();
void ShowSettings();
