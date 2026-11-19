#include "MemoryEngine.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vram {

namespace {

using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS_NT = 0x00000000L;
constexpr ULONG SystemMemoryListInformation             = 80;
constexpr ULONG SystemCombinePhysicalMemoryInformation  = 130;
constexpr ULONG SystemRegistryReconciliationInformation = 155;
constexpr ULONG NtProcessIoPriority                     = 33;

constexpr DWORD kWinProcessMemoryPriority = 0;
constexpr DWORD kWinProcessPowerThrottling = 4;
constexpr ULONG kMemoryPriorityNormalValue = 5;
constexpr ULONG kPowerThrottlingVersion1 = 1;
constexpr ULONG kPowerThrottlingExecutionSpeed = 0x1;

struct MEMORY_COMBINE_INFORMATION_EX {
    HANDLE Handle;
    ULONG_PTR PagesCombined;
    ULONG Flags;
};

struct RAW_MEMORY_PRIORITY_INFO {
    ULONG MemoryPriority;
};

struct RAW_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
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

using PfnNtSetInformationProcess = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength
);

using PfnSetProcessInformation = BOOL(WINAPI*)(
    HANDLE hProcess,
    DWORD ProcessInformationClass,
    LPVOID ProcessInformation,
    DWORD ProcessInformationSize
);

std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

bool IsProtectedSystemExe(const wchar_t* exeName) {
    if (!exeName || !*exeName) return true;
    static constexpr const wchar_t* kSystemProtected[] = {
        L"System",
        L"Registry",
        L"Memory Compression",
        L"vmmem",
        L"vmmemWSL",
        L"smss.exe",
        L"csrss.exe",
        L"wininit.exe",
        L"services.exe",
        L"lsass.exe",
        L"lsaiso.exe",
        L"winlogon.exe",
        L"fontdrvhost.exe",
        L"dwm.exe",
        L"audiodg.exe",
        L"svchost.exe",
        L"sihost.exe",
        L"SecurityHealthService.exe",
        L"MsMpEng.exe",
        L"VRam.exe"
    };
    for (const wchar_t* entry : kSystemProtected) {
        if (_wcsicmp(exeName, entry) == 0) {
            return true;
        }
    }
    return false;
}

bool MatchesProtectedGame(const wchar_t* exeName, const std::vector<std::wstring>& protectedGames) {
    if (!exeName || !*exeName) return false;
    for (const auto& game : protectedGames) {
        if (!game.empty() && _wcsicmp(exeName, game.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

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

uint32_t TrimAccessibleWorkingSets(const std::vector<std::wstring>& protectedGames) {
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
            if (IsProtectedSystemExe(pe.szExeFile) || MatchesProtectedGame(pe.szExeFile, protectedGames)) {
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

struct WindowPidSet {
    std::unordered_set<DWORD> pids;
};

BOOL CALLBACK CollectVisibleWindowPids(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        reinterpret_cast<WindowPidSet*>(lParam)->pids.insert(pid);
    }
    return TRUE;
}

bool BoostSingleGameProcess(DWORD pid, uint64_t totalPhysBytes, uint64_t availPhysBytes,
                            PfnSetProcessInformation pSetProcInfo,
                            PfnNtSetInformationProcess pNtSetInfoProc) {
    HANDLE hProc = OpenProcess(
        PROCESS_SET_INFORMATION | PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid
    );
    if (!hProc) {
        hProc = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }
    if (!hProc) {
        return false;
    }

    bool anyApplied = false;

    const DWORD currentClass = GetPriorityClass(hProc);
    if (currentClass == NORMAL_PRIORITY_CLASS ||
        currentClass == BELOW_NORMAL_PRIORITY_CLASS ||
        currentClass == IDLE_PRIORITY_CLASS) {
        if (SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS)) {
            anyApplied = true;
        }
    } else if (currentClass != 0) {
        anyApplied = true;
    }

    SetProcessPriorityBoost(hProc, FALSE);

    if (pSetProcInfo) {
        RAW_MEMORY_PRIORITY_INFO memPrio{kMemoryPriorityNormalValue};
        if (pSetProcInfo(hProc, kWinProcessMemoryPriority, &memPrio, sizeof(memPrio))) {
            anyApplied = true;
        }

        RAW_POWER_THROTTLING_STATE throttle{};
        throttle.Version = kPowerThrottlingVersion1;
        throttle.ControlMask = kPowerThrottlingExecutionSpeed;
        throttle.StateMask = 0;
        if (pSetProcInfo(hProc, kWinProcessPowerThrottling, &throttle, sizeof(throttle))) {
            anyApplied = true;
        }
    }

    if (pNtSetInfoProc) {
        ULONG highIo = 3;
        pNtSetInfoProc(hProc, NtProcessIoPriority, &highIo, sizeof(highIo));
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc)) && totalPhysBytes > 0) {
        const SIZE_T currentWs = pmc.WorkingSetSize;
        SIZE_T curMin = 0;
        SIZE_T curMax = 0;
        DWORD curFlags = 0;
        GetProcessWorkingSetSizeEx(hProc, &curMin, &curMax, &curFlags);

        constexpr uint64_t kReserveForOs = 1536ULL * 1024ULL * 1024ULL;
        const uint64_t safeCeiling = (totalPhysBytes > kReserveForOs)
            ? (totalPhysBytes - kReserveForOs)
            : (totalPhysBytes / 2);
        const uint64_t headRoomCap = currentWs + (availPhysBytes / 2);
        const uint64_t desiredMin = std::min<uint64_t>(
            std::max<uint64_t>(curMin, currentWs + 128ULL * 1024ULL * 1024ULL),
            std::min<uint64_t>(safeCeiling / 2, headRoomCap)
        );
        const uint64_t desiredMax = std::max<uint64_t>(
            desiredMin + 512ULL * 1024ULL * 1024ULL,
            std::min<uint64_t>(safeCeiling, std::max<uint64_t>(curMax, currentWs + availPhysBytes))
        );

        if (desiredMin > 0 && desiredMax > desiredMin) {
            SetProcessWorkingSetSizeEx(
                hProc,
                static_cast<SIZE_T>(desiredMin),
                static_cast<SIZE_T>(desiredMax),
                QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE
            );
        }
    }

    CloseHandle(hProc);
    return anyApplied;
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
    EnablePrivilege(hToken, SE_INC_BASE_PRIORITY_NAME);
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

    const ProtectedGameStatus preGameStatus =
        QueryAndApplyGameProtection(options.protectedGames, options.maximizeGameRam);

    const MemorySnapshot before = Query();
    PurgeResult result{};
    result.beforeUsedBytes = before.usedBytes;
    result.beforePercent = before.usagePercent;
    result.protectedProcesses = preGameStatus.runningInstanceCount;
    result.activeGameName = preGameStatus.primaryGameName;
    result.activeGameRamBytes = preGameStatus.combinedWorkingSetBytes;

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

    const bool skipGlobalWorkingSetEmpty = preGameStatus.anyRunning;

    if (options.workingSets) {
        if (!skipGlobalWorkingSetEmpty) {
            runListCmd(MemoryEmptyWorkingSets);
        }
        result.trimmedProcesses = TrimAccessibleWorkingSets(options.protectedGames);
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
        if (!skipGlobalWorkingSetEmpty) {
            runListCmd(MemoryEmptyWorkingSets);
        }
        TrimAccessibleWorkingSets(options.protectedGames);
    }
    if (options.systemCache) {
        SetSystemFileCacheSize(static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1), 0);
    }
    if (options.standbyList) {
        runListCmd(MemoryPurgeStandbyList);
    }

    if (preGameStatus.anyRunning) {
        const ProtectedGameStatus postGameStatus =
            QueryAndApplyGameProtection(options.protectedGames, options.maximizeGameRam);
        result.activeGameRamBytes = postGameStatus.combinedWorkingSetBytes;
        result.protectedProcesses = postGameStatus.runningInstanceCount;
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

std::vector<RunningProcessInfo> MemoryEngine::ListRunningProcesses() {
    WindowPidSet winPids{};
    EnumWindows(CollectVisibleWindowPids, reinterpret_cast<LPARAM>(&winPids));

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::unordered_map<std::wstring, RunningProcessInfo> grouped;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    const DWORD selfPid = GetCurrentProcessId();

    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == 4 || pe.th32ProcessID == selfPid) {
                continue;
            }
            if (IsProtectedSystemExe(pe.szExeFile)) {
                continue;
            }

            uint64_t wsBytes = 0;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    wsBytes = pmc.WorkingSetSize;
                }
                CloseHandle(hProc);
            }

            const std::wstring key = ToLowerCopy(pe.szExeFile);
            auto& entry = grouped[key];
            if (entry.exeName.empty()) {
                entry.exeName = pe.szExeFile;
            }
            entry.instanceCount += 1;
            entry.totalWorkingSetBytes += wsBytes;
            if (winPids.pids.find(pe.th32ProcessID) != winPids.pids.end()) {
                entry.hasWindow = true;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);

    std::vector<RunningProcessInfo> result;
    result.reserve(grouped.size());
    for (auto& kv : grouped) {
        result.push_back(std::move(kv.second));
    }

    std::sort(result.begin(), result.end(), [](const RunningProcessInfo& a, const RunningProcessInfo& b) {
        if (a.hasWindow != b.hasWindow) return a.hasWindow > b.hasWindow;
        if (a.totalWorkingSetBytes != b.totalWorkingSetBytes) return a.totalWorkingSetBytes > b.totalWorkingSetBytes;
        return _wcsicmp(a.exeName.c_str(), b.exeName.c_str()) < 0;
    });

    return result;
}

ProtectedGameStatus MemoryEngine::QueryAndApplyGameProtection(const std::vector<std::wstring>& protectedGames,
                                                              bool applyBoost) {
    ProtectedGameStatus status{};
    if (protectedGames.empty()) {
        return status;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return status;
    }

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    uint64_t totalPhys = 0;
    uint64_t availPhys = 0;
    if (GlobalMemoryStatusEx(&mem)) {
        totalPhys = mem.ullTotalPhys;
        availPhys = mem.ullAvailPhys;
    }

    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    auto pSetProcInfo = hKernel
        ? reinterpret_cast<PfnSetProcessInformation>(GetProcAddress(hKernel, "SetProcessInformation"))
        : nullptr;
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    auto pNtSetInfoProc = hNtDll
        ? reinterpret_cast<PfnNtSetInformationProcess>(GetProcAddress(hNtDll, "NtSetInformationProcess"))
        : nullptr;

    std::wstring bestName;
    uint64_t bestGameRam = 0;
    std::unordered_map<std::wstring, uint64_t> ramByExe;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!MatchesProtectedGame(pe.szExeFile, protectedGames)) {
                continue;
            }
            status.anyRunning = true;
            status.runningInstanceCount += 1;

            uint64_t wsBytes = 0;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc{};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    wsBytes = pmc.WorkingSetSize;
                }
                CloseHandle(hProc);
            }
            status.combinedWorkingSetBytes += wsBytes;
            const std::wstring key = ToLowerCopy(pe.szExeFile);
            ramByExe[key] += wsBytes;
            if (ramByExe[key] >= bestGameRam) {
                bestGameRam = ramByExe[key];
                bestName = pe.szExeFile;
            }

            if (applyBoost) {
                if (BoostSingleGameProcess(pe.th32ProcessID, totalPhys, availPhys, pSetProcInfo, pNtSetInfoProc)) {
                    status.boosted = true;
                }
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    status.primaryGameName = std::move(bestName);
    return status;
}

std::wstring MemoryEngine::FormatBytes(uint64_t bytes) {
    wchar_t buf[64]{};
    constexpr double kKB = 1024.0;
    constexpr double kMB = kKB * 1024.0;
    constexpr double kGB = kMB * 1024.0;

    const double val = static_cast<double>(bytes);
    if (val >= kGB) {
        _snwprintf_s(buf, _TRUNCATE, L"%.1f GB", val / kGB);
    } else if (val >= kMB) {
        _snwprintf_s(buf, _TRUNCATE, L"%.0f MB", val / kMB);
    } else if (val >= kKB) {
        _snwprintf_s(buf, _TRUNCATE, L"%.0f KB", val / kKB);
    } else {
        _snwprintf_s(buf, _TRUNCATE, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

}
