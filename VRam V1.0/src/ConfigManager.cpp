#include "ConfigManager.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace vram {

namespace {

constexpr wchar_t kRunSubKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"VRam";

bool ParseBool(const wchar_t* val, bool fallback) {
    if (!val) return fallback;
    if (_wcsicmp(val, L"true") == 0 || _wcsicmp(val, L"1") == 0) return true;
    if (_wcsicmp(val, L"false") == 0 || _wcsicmp(val, L"0") == 0) return false;
    return fallback;
}

int ParseInt(const wchar_t* val, int fallback) {
    if (!val || !*val) return fallback;
    wchar_t* end = nullptr;
    long v = wcstol(val, &end, 10);
    if (end == val) return fallback;
    return static_cast<int>(v);
}

std::wstring GetExePathQuoted(bool startMinimized) {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    std::wstring cmd = L"\"";
    cmd += exePath;
    cmd += L"\"";
    if (startMinimized) {
        cmd += L" --minimized";
    }
    return cmd;
}

}

std::wstring ConfigManager::GetConfigFilePath() {
    wchar_t* appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        dir = appData;
        CoTaskMemFree(appData);
    } else {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PathRemoveFileSpecW(exePath);
        dir = exePath;
    }

    dir += L"\\VRam";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\config.xml";
}

AppConfig ConfigManager::Load() {
    AppConfig cfg{};
    const std::wstring path = GetConfigFilePath();

    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                      FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream))) {
        cfg.autoStart = IsAutoStartupEnabled();
        return cfg;
    }

    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr))) {
        cfg.autoStart = IsAutoStartupEnabled();
        return cfg;
    }

    if (FAILED(reader->SetInput(stream.Get()))) {
        cfg.autoStart = IsAutoStartupEnabled();
        return cfg;
    }

    XmlNodeType nodeType = XmlNodeType_None;
    while (reader->Read(&nodeType) == S_OK) {
        if (nodeType != XmlNodeType_Element) {
            continue;
        }
        const wchar_t* elemName = nullptr;
        if (FAILED(reader->GetLocalName(&elemName, nullptr)) || !elemName) {
            continue;
        }

        if (wcscmp(elemName, L"Startup") == 0) {
            for (HRESULT hr = reader->MoveToFirstAttribute(); hr == S_OK; hr = reader->MoveToNextAttribute()) {
                const wchar_t* name = nullptr;
                const wchar_t* val = nullptr;
                reader->GetLocalName(&name, nullptr);
                reader->GetValue(&val, nullptr);
                if (!name || !val) continue;
                if (wcscmp(name, L"autoStart") == 0) cfg.autoStart = ParseBool(val, cfg.autoStart);
                else if (wcscmp(name, L"startMinimized") == 0) cfg.startMinimized = ParseBool(val, cfg.startMinimized);
            }
        } else if (wcscmp(elemName, L"Window") == 0) {
            for (HRESULT hr = reader->MoveToFirstAttribute(); hr == S_OK; hr = reader->MoveToNextAttribute()) {
                const wchar_t* name = nullptr;
                const wchar_t* val = nullptr;
                reader->GetLocalName(&name, nullptr);
                reader->GetValue(&val, nullptr);
                if (!name || !val) continue;
                if (wcscmp(name, L"minimizeToTray") == 0) cfg.minimizeToTray = ParseBool(val, cfg.minimizeToTray);
                else if (wcscmp(name, L"closeToTray") == 0) cfg.closeToTray = ParseBool(val, cfg.closeToTray);
                else if (wcscmp(name, L"alwaysOnTop") == 0) cfg.alwaysOnTop = ParseBool(val, cfg.alwaysOnTop);
                else if (wcscmp(name, L"trayStyle") == 0) cfg.trayStyle = (wcscmp(val, L"percent") == 0) ? L"percent" : L"logo";
            }
        } else if (wcscmp(elemName, L"Purge") == 0) {
            for (HRESULT hr = reader->MoveToFirstAttribute(); hr == S_OK; hr = reader->MoveToNextAttribute()) {
                const wchar_t* name = nullptr;
                const wchar_t* val = nullptr;
                reader->GetLocalName(&name, nullptr);
                reader->GetValue(&val, nullptr);
                if (!name || !val) continue;
                if (wcscmp(name, L"workingSets") == 0) cfg.purgeWorkingSets = ParseBool(val, cfg.purgeWorkingSets);
                else if (wcscmp(name, L"systemCache") == 0) cfg.purgeSystemCache = ParseBool(val, cfg.purgeSystemCache);
                else if (wcscmp(name, L"standbyList") == 0) cfg.purgeStandbyList = ParseBool(val, cfg.purgeStandbyList);
                else if (wcscmp(name, L"modifiedList") == 0) cfg.purgeModifiedList = ParseBool(val, cfg.purgeModifiedList);
            }
        } else if (wcscmp(elemName, L"Automation") == 0) {
            for (HRESULT hr = reader->MoveToFirstAttribute(); hr == S_OK; hr = reader->MoveToNextAttribute()) {
                const wchar_t* name = nullptr;
                const wchar_t* val = nullptr;
                reader->GetLocalName(&name, nullptr);
                reader->GetValue(&val, nullptr);
                if (!name || !val) continue;
                if (wcscmp(name, L"autoPurgeEnabled") == 0) cfg.autoPurgeEnabled = ParseBool(val, cfg.autoPurgeEnabled);
                else if (wcscmp(name, L"thresholdPercent") == 0) {
                    cfg.autoPurgeThreshold = std::clamp(ParseInt(val, cfg.autoPurgeThreshold), 50, 95);
                } else if (wcscmp(name, L"intervalMinutes") == 0) {
                    cfg.autoPurgeIntervalMinutes = std::clamp(ParseInt(val, cfg.autoPurgeIntervalMinutes), 0, 1440);
                } else if (wcscmp(name, L"showNotifications") == 0) {
                    cfg.showNotifications = ParseBool(val, cfg.showNotifications);
                }
            }
        }
    }

    cfg.autoStart = IsAutoStartupEnabled();
    return cfg;
}

bool ConfigManager::Save(const AppConfig& cfg) {
    const std::wstring path = GetConfigFilePath();

    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
                                      FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) {
        return false;
    }

    ComPtr<IXmlWriter> writer;
    if (FAILED(CreateXmlWriter(__uuidof(IXmlWriter), reinterpret_cast<void**>(writer.GetAddressOf()), nullptr))) {
        return false;
    }

    if (FAILED(writer->SetOutput(stream.Get()))) {
        return false;
    }
    writer->SetProperty(XmlWriterProperty_Indent, TRUE);

    auto bstr = [](bool v) { return v ? L"true" : L"false"; };

    writer->WriteStartDocument(XmlStandalone_Omit);
    writer->WriteStartElement(nullptr, L"VRamConfig", nullptr);
    writer->WriteAttributeString(nullptr, L"version", nullptr, L"1.0");

    writer->WriteStartElement(nullptr, L"Startup", nullptr);
    writer->WriteAttributeString(nullptr, L"autoStart", nullptr, bstr(cfg.autoStart));
    writer->WriteAttributeString(nullptr, L"startMinimized", nullptr, bstr(cfg.startMinimized));
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"Window", nullptr);
    writer->WriteAttributeString(nullptr, L"minimizeToTray", nullptr, bstr(cfg.minimizeToTray));
    writer->WriteAttributeString(nullptr, L"closeToTray", nullptr, bstr(cfg.closeToTray));
    writer->WriteAttributeString(nullptr, L"alwaysOnTop", nullptr, bstr(cfg.alwaysOnTop));
    writer->WriteAttributeString(nullptr, L"trayStyle", nullptr, cfg.trayStyle.c_str());
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"Purge", nullptr);
    writer->WriteAttributeString(nullptr, L"workingSets", nullptr, bstr(cfg.purgeWorkingSets));
    writer->WriteAttributeString(nullptr, L"systemCache", nullptr, bstr(cfg.purgeSystemCache));
    writer->WriteAttributeString(nullptr, L"standbyList", nullptr, bstr(cfg.purgeStandbyList));
    writer->WriteAttributeString(nullptr, L"modifiedList", nullptr, bstr(cfg.purgeModifiedList));
    writer->WriteEndElement();

    writer->WriteStartElement(nullptr, L"Automation", nullptr);
    writer->WriteAttributeString(nullptr, L"autoPurgeEnabled", nullptr, bstr(cfg.autoPurgeEnabled));
    writer->WriteAttributeString(nullptr, L"thresholdPercent", nullptr, std::to_wstring(cfg.autoPurgeThreshold).c_str());
    writer->WriteAttributeString(nullptr, L"intervalMinutes", nullptr, std::to_wstring(cfg.autoPurgeIntervalMinutes).c_str());
    writer->WriteAttributeString(nullptr, L"showNotifications", nullptr, bstr(cfg.showNotifications));
    writer->WriteEndElement();

    writer->WriteEndElement();
    writer->WriteEndDocument();
    writer->Flush();
    return true;
}

bool ConfigManager::IsAutoStartupEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG status = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t);
}

bool ConfigManager::SetAutoStartup(bool enabled, bool startMinimized) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG status = ERROR_SUCCESS;
    if (enabled) {
        std::wstring cmd = GetExePathQuoted(startMinimized);
        if (cmd.empty()) {
            RegCloseKey(hKey);
            return false;
        }
        const DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        status = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(cmd.c_str()), bytes);
    } else {
        status = RegDeleteValueW(hKey, kRunValueName);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }

    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

}
