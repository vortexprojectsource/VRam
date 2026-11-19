#pragma once

#include <string>
#include <vector>

namespace vram {

struct AppConfig {
    bool autoStart = false;
    bool startMinimized = false;
    bool minimizeToTray = true;
    bool closeToTray = true;
    bool alwaysOnTop = false;
    std::wstring trayStyle = L"logo";

    bool purgeWorkingSets = true;
    bool purgeSystemCache = true;
    bool purgeStandbyList = true;
    bool purgeModifiedList = true;

    bool autoPurgeEnabled = false;
    int autoPurgeThreshold = 80;
    int autoPurgeIntervalMinutes = 0;
    bool showNotifications = true;

    bool maximizeGameRam = true;
    std::vector<std::wstring> protectedGames;
};

class ConfigManager {
public:
    static std::wstring GetConfigFilePath();
    static AppConfig Load();
    static bool Save(const AppConfig& cfg);

    static bool IsAutoStartupEnabled();
    static bool SetAutoStartup(bool enabled, bool startMinimized);
};

}
