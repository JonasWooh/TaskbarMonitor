#pragma once

#include <windows.h>

// Minimal client for the PawnIO driver (https://pawnio.eu): open the device,
// load one signed module, and call its ioctl functions.
class PawnIo {
public:
    enum class Result { Ok, NotInstalled, AccessDenied, LoadFailed };

    Result Open(const void* module, DWORD moduleSize);
    bool Execute(const char* fn, const ULONG64* in, DWORD inCount, ULONG64* out, DWORD outCount);
    void Close();
    ~PawnIo() { Close(); }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
