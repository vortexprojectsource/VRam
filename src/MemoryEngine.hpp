#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vram {

struct MemorySnapshot {
    uint32_t usagePercent = 0;
    uint64_t totalBytes = 0;
    uint64_t availBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t cachedBytes = 0;
    uint64_t commitTotalBytes = 0;
    uint64_t commitLimitBytes = 0;
    uint32_t processCount = 0;
};

struct PurgeOptions {
    bool workingSets = true;
    bool systemCache = true;
    bool standbyList = true;
    bool modifiedList = false;
    bool maximizeGameRam = true;
    std::vector<std::wstring> protectedGames;
};

struct PurgeResult {
    uint64_t beforeUsedBytes = 0;
    uint64_t afterUsedBytes = 0;
    uint64_t freedBytes = 0;
    uint32_t beforePercent = 0;
    uint32_t afterPercent = 0;
    uint32_t trimmedProcesses = 0;
    uint32_t protectedProcesses = 0;
    std::wstring activeGameName;
    uint64_t activeGameRamBytes = 0;
    bool kernelPurgeSucceeded = false;
    bool privilegeLimited = false;
};

struct RunningProcessInfo {
    std::wstring exeName;
    uint32_t instanceCount = 0;
    uint64_t totalWorkingSetBytes = 0;
    bool hasWindow = false;
};

struct ProtectedGameStatus {
    bool anyRunning = false;
    std::wstring primaryGameName;
    uint32_t runningInstanceCount = 0;
    uint64_t combinedWorkingSetBytes = 0;
    bool boosted = false;
};

class MemoryEngine {
public:
    static bool IsElevated();
    static bool RestartElevated(bool startMinimized);
    static void EnableAvailablePrivileges();
    static MemorySnapshot Query();
    static PurgeResult Purge(const PurgeOptions& options);
    static std::vector<RunningProcessInfo> ListRunningProcesses();
    static ProtectedGameStatus QueryAndApplyGameProtection(const std::vector<std::wstring>& protectedGames,
                                                           bool applyBoost);
    static std::wstring FormatBytes(uint64_t bytes);
};

}
