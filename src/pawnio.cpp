#include "pawnio.h"

#include <string.h>

namespace {
constexpr DWORD kDeviceType = 41394u << 16;
constexpr DWORD kIoctlLoadBinary = kDeviceType | (0x821 << 2);
constexpr DWORD kIoctlExecute = kDeviceType | (0x841 << 2);
constexpr DWORD kFnNameLength = 32;
constexpr DWORD kMaxArgs = 8;
}  // namespace

PawnIo::Result PawnIo::Open(const void* module, DWORD moduleSize) {
    Close();
    handle_ = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_ACCESS_DENIED ? Result::AccessDenied : Result::NotInstalled;
    DWORD ret = 0;
    if (!DeviceIoControl(handle_, kIoctlLoadBinary, const_cast<void*>(module), moduleSize, nullptr, 0, &ret, nullptr)) {
        Close();
        return Result::LoadFailed;
    }
    return Result::Ok;
}

bool PawnIo::Execute(const char* fn, const ULONG64* in, DWORD inCount, ULONG64* out, DWORD outCount) {
    if (handle_ == INVALID_HANDLE_VALUE || inCount > kMaxArgs) return false;
    unsigned char buf[kFnNameLength + kMaxArgs * sizeof(ULONG64)] = {};
    strncpy(reinterpret_cast<char*>(buf), fn, kFnNameLength - 1);
    memcpy(buf + kFnNameLength, in, inCount * sizeof(ULONG64));
    DWORD ret = 0;
    if (!DeviceIoControl(handle_, kIoctlExecute, buf, kFnNameLength + inCount * sizeof(ULONG64), out,
                         outCount * sizeof(ULONG64), &ret, nullptr))
        return false;
    return ret == outCount * sizeof(ULONG64);
}

void PawnIo::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
}
