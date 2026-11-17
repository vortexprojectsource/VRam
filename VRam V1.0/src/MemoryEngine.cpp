#include "MemoryEngine.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <vector>

namespace vram {

namespace {

using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS_NT = 0x00000000L;
constexpr ULONG SystemMemoryListInformation            = 80;
constexpr ULONG SystemCombinePhysicalMemoryInformation = 130;
constexpr ULONG SystemRegistryReconciliationInformation = 155;

struct MEMORY_COMBINE_INFORMATION_EX {
    HANDLE Handle;
    ULONG_PTR PagesCombined;
    ULONG Flags;
};

enum SYSTEM_MEMORY_LIST_COMMAND : ULONG {
    MemoryCaptureAccessedBits = 0,
    MemoryCaptureAndResetAccessedBits = 1,
    MemoryEmptyWorkingSets = 2,
    MemoryFlushModifiedList = 3,
    MemoryPurgeStandbyList = 4,
    MemoryPurgeLowPriorityStandbyList = 5
};

using PfnNtSetSystemInformation = NTSTATUS(NTAPI*)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength
);

bool EnablePrivilege(HANDLE hToken, const wchar_t* privilegeName) {
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        return false;
    }
    return GetLastError() == ERROR_SUCCESS;
}

uint32_t TrimAccessibleWorkingSets() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }

    const DWORD selfPid = GetCurrentProcessId();
    uint32_t trimmed = 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4 || pe.th32ProcessID == selfPid) {
                continue;
            }
            HANDLE hProc = OpenProcess(
                PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                pe.th32ProcessID
            );
            if (!hProc) {
                continue;
            }
            const BOOL bSet = SetProcessWorkingSetSizeEx(
                hProc,
                static_cast<SIZE_T>(-1),
                static_cast<SIZE_T>(-1),
                0
            );
            const BOOL bEmpty = EmptyWorkingSet(hProc);
            if (bSet || bEmpty) {
                ++trimmed;
            }
            CloseHandle(hProc);
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);

    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSizeEx(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    EmptyWorkingSet(GetCurrentProcess());
    return trimmed;
}

}

bool MemoryEngine::IsElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD retLen = 0;
    BOOL ok = GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &retLen);
    CloseHandle(hToken);
    return ok && elevation.TokenIsElevated != 0;
}

bool MemoryEngine::RestartElevated(bool startMinimized) {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = startMinimized ? L"--minimized --elevated-restart" : L"--elevated-restart";
    sei.nShow = startMinimized ? SW_HIDE : SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
}

void MemoryEngine::EnableAvailablePrivileges() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return;
    }
    EnablePrivilege(hToken, SE_INCREASE_QUOTA_NAME);
    EnablePrivilege(hToken, SE_PROF_SINGLE_PROCESS_NAME);
    EnablePrivilege(hToken, SE_DEBUG_NAME);
    CloseHandle(hToken);
}

MemorySnapshot MemoryEngine::Query() {
    MemorySnapshot snap{};

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        snap.usagePercent = mem.dwMemoryLoad;
        snap.totalBytes = mem.ullTotalPhys;
        snap.availBytes = mem.ullAvailPhys;
        snap.usedBytes = (mem.ullTotalPhys >= mem.ullAvailPhys)
            ? (mem.ullTotalPhys - mem.ullAvailPhys)
            : 0;
    }

    PERFORMANCE_INFORMATION perf{};
    perf.cb = sizeof(perf);
    if (GetPerformanceInfo(&perf, sizeof(perf))) {
        const uint64_t pageSize = static_cast<uint64_t>(perf.PageSize);
        snap.cachedBytes = static_cast<uint64_t>(perf.SystemCache) * pageSize;
        snap.commitTotalBytes = static_cast<uint64_t>(perf.CommitTotal) * pageSize;
        snap.commitLimitBytes = static_cast<uint64_t>(perf.CommitLimit) * pageSize;
        snap.processCount = perf.ProcessCount;
    }

    return snap;
}

PurgeResult MemoryEngine::Purge(const PurgeOptions& options) {
    EnableAvailablePrivileges();

    const MemorySnapshot before = Query();
    PurgeResult result{};
    result.beforeUsedBytes = before.usedBytes;
    result.beforePercent = before.usagePercent;

    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    auto pNtSetSystemInformation = hNtDll
        ? reinterpret_cast<PfnNtSetSystemInformation>(GetProcAddress(hNtDll, "NtSetSystemInformation"))
        : nullptr;

    auto runListCmd = [&](SYSTEM_MEMORY_LIST_COMMAND cmd) -> bool {
        if (!pNtSetSystemInformation) {
            return false;
        }
        ULONG value = static_cast<ULONG>(cmd);
        NTSTATUS st = pNtSetSystemInformation(SystemMemoryListInformation, &value, sizeof(value));
        if (st == STATUS_SUCCESS_NT) {
            result.kernelPurgeSucceeded = true;
            return true;
        }
        result.privilegeLimited = true;
        return false;
    };

    if (options.workingSets) {
        runListCmd(MemoryEmptyWorkingSets);
        result.trimmedProcesses = TrimAccessibleWorkingSets();
    }

    if (options.systemCache) {
        if (SetSystemFileCacheSize(static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0)) {
            result.kernelPurgeSucceeded = true;
        } else {
            result.privilegeLimited = true;
        }
        if (pNtSetSystemInformation) {
            pNtSetSystemInformation(SystemRegistryReconciliationInformation, nullptr, 0);
        }
    }

    if (options.modifiedList) {
        runListCmd(MemoryFlushModifiedList);
    }

    if (pNtSetSystemInformation) {
        MEMORY_COMBINE_INFORMATION_EX combineInfo{};
        if (pNtSetSystemInformation(SystemCombinePhysicalMemoryInformation, &combineInfo, sizeof(combineInfo)) == STATUS_SUCCESS_NT) {
            result.kernelPurgeSucceeded = true;
        }
    }

    if (options.standbyList) {
        runListCmd(MemoryPurgeLowPriorityStandbyList);
        runListCmd(MemoryPurgeStandbyList);
    }

    Sleep(70);
    if (options.workingSets) {
        runListCmd(MemoryEmptyWorkingSets);
        TrimAccessibleWorkingSets();
    }
    if (options.systemCache) {
        SetSystemFileCacheSize(static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    }
    if (options.standbyList) {
        runListCmd(MemoryPurgeStandbyList);
    }

    HeapCompact(GetProcessHeap(), 0);
    EmptyWorkingSet(GetCurrentProcess());
    Sleep(80);

    const MemorySnapshot after = Query();
    result.afterUsedBytes = after.usedBytes;
    result.afterPercent = after.usagePercent;
    result.freedBytes = (before.usedBytes > after.usedBytes)
        ? (before.usedBytes - after.usedBytes)
        : 0;

    return result;
}

std::wstring MemoryEngine::FormatBytes(uint64_t bytes) {
    wchar_t buf[64]{};
    constexpr double kKB = 1024.0;
    constexpr double kMB = kKB * 1024.0;
    constexpr double kGB = kMB * 1024.0;

    const double val = static_cast<double>(bytes);
    if (val >= kGB) {
        std::swprintf(buf, 64, L"%.1f GB", val / kGB);
    } else if (val >= kMB) {
        std::swprintf(buf, 64, L"%.0f MB", val / kMB);
    } else if (val >= kKB) {
        std::swprintf(buf, 64, L"%.0f KB", val / kKB);
    } else {
        std::swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

}
