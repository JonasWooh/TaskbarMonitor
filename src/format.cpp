#include "format.h"

#include <stdio.h>
#include <wchar.h>

void FmtPct(wchar_t* b, double v) {
    if (v < 0) wcscpy(b, L"--%"); else swprintf(b, 16, L"%.0f%%", v);
}
void FmtGHz(wchar_t* b, double mhz) {
    if (mhz < 0) wcscpy(b, L"-- GHz"); else swprintf(b, 16, L"%.2f GHz", mhz / 1000.0);
}
void FmtTemp(wchar_t* b, double c) {
    if (c < 0) wcscpy(b, L"--°C"); else swprintf(b, 16, L"%.0f°C", c);
}
void FmtWatt(wchar_t* b, double w) {
    if (w < 0) wcscpy(b, L"-- W");
    else swprintf(b, 16, w < 10 ? L"%.1f W" : L"%.0f W", w);
}
void FmtGB(wchar_t* b, double gb) {
    if (gb < 0) wcscpy(b, L"-- GB");
    else swprintf(b, 16, gb < 100 ? L"%.1f GB" : L"%.0f GB", gb);
}
void FmtRate(wchar_t* b, double bps) {
    if (bps < 0) { wcscpy(b, L"-- KB/s"); return; }
    static const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
    int u = 0;
    while (bps >= 1000 && u < 3) { bps /= 1024; ++u; }
    if (u == 0) swprintf(b, 16, L"%.0f %ls", bps, units[u]);
    else swprintf(b, 16, bps < 10 ? L"%.2f %ls" : bps < 100 ? L"%.1f %ls" : L"%.0f %ls", bps, units[u]);
}
