#pragma once

#include <windows.h>

// Where the taskbar's own content (Start, search, task buttons, widgets…)
// actually ends. Windows 11 draws these in XAML without HWNDs, so this asks
// UI Automation. Call from a COM MTA thread (the sampling thread), never from
// the UI thread, which shares explorer's input queue.
namespace tbspace {

// Right edge in screen pixels of the right-most taskbar element that starts
// left of `limitX` (the notification area), or -1 if it can't be determined.
int OccupiedRight(HWND taskbar, int limitX);

void Shutdown();

}  // namespace tbspace
