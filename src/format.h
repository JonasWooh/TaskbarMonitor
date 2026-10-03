#pragma once

// Value formatting shared by the strip, tooltip and panel. Buffers must hold
// at least 16 characters; negative inputs mean "not available" and give "--".
void FmtPct(wchar_t* b, double v);
void FmtGHz(wchar_t* b, double mhz);
void FmtTemp(wchar_t* b, double c);
void FmtWatt(wchar_t* b, double w);
void FmtGB(wchar_t* b, double gb);
void FmtRate(wchar_t* b, double bytesPerSec);
