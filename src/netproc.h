#pragma once

#include <windows.h>

// Per-process network throughput from the Microsoft-Windows-Kernel-Network
// ETW provider (the same source Resource Monitor uses). A private real-time
// session runs only while someone is looking at the numbers.
namespace netproc {

enum class State { Off, Running, NeedsAdmin, Failed };

struct Entry {
    double down, up;           // bytes/s over the last interval
    int processes;             // PIDs merged into this entry (same image)
    wchar_t name[64];          // image file name
    wchar_t path[MAX_PATH];    // full image path, empty for System
};

// Start/Stop are cheap to call repeatedly; call from one thread.
State Start();
void Stop();
State GetState();

// Computes rates since the previous call and returns the busiest images,
// sorted by down+up. Call about once per second while running.
int Collect(Entry* out, int max);

}  // namespace netproc
