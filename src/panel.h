#pragma once

#include <windows.h>

// The flyout opened by left-clicking the strip: a two-column overview of all
// metrics with 60 s history, or the network view with per-process traffic.
namespace panel {

enum class Mode { Overview, Network };

// Opens the panel above `anchor` (screen rect of the clicked strip area),
// switches mode if it is open in the other mode, or closes it.
void Toggle(Mode mode, const RECT& anchor);
void Close();
bool IsOpen();

void OnData();          // a new sample is available
void OnThemeChanged();  // light/dark or accent color changed

}  // namespace panel
