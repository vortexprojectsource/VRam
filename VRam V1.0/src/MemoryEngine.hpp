#pragma once

#include <cstdint>
#include <string>

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
};

struct PurgeResult {
    uint64_t beforeUsedBytes = 0;
    uint64_t afterUsedBytes = 0;
    uint64_t freedBytes = 0;
    uint32_t beforePercent = 0;
    uint32_t afterPercent = 0;
    uint32_t trimmedProcesses = 0;
    bool kernelPurgeSucceeded = false;
    bool privilegeLimited = false;
};

class MemoryEngine {
public:
    static bool IsElevated();
    static bool RestartElevated(bool startMinimized);
    static void EnableAvailablePrivileges();
    static MemorySnapshot Query();
    static PurgeResult Purge(const PurgeOptions& options);
    static std::wstring FormatBytes(uint64_t bytes);
};

}
