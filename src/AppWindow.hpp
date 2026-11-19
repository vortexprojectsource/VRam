#pragma once

#include "ConfigManager.hpp"
#include "MemoryEngine.hpp"
#include "PurgeToastWindow.hpp"
#include "TrayIconRenderer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <unknwn.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace vram {

constexpr UINT WM_APP_SHOW_WINDOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY_ICON   = WM_APP + 2;
constexpr UINT WM_APP_PURGE_DONE  = WM_APP + 3;

class AppWindow {
public:
    AppWindow(HINSTANCE hInst, bool startMinimized);
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool Create();
    bool PreTranslateMessage(MSG* msg);
    HWND GetHwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnDestroy();
    void OnSize(UINT width, UINT height);
    void OnDpiChanged(UINT newDpi, const RECT* suggestedRect);
    void OnTimer(UINT_PTR timerId);
    void OnTrayMessage(LPARAM lParam);
    void OnPurgeFinished(PurgeResult* result);

    void ApplyDwmTheme();
    void ApplyAlwaysOnTop();
    void ResizeXamlIsland();

    void BindXamlControls();
    void LoadLogoSvgIntoUi();
    void HydrateSettingsUi();
    void UpdateCustomOptionPills();
    void SaveAndApplyConfig();

    enum class ActiveView { Main, Settings, Games };
    void SwitchToView(ActiveView view);
    void ShowMainWindow();
    void HideToTray();
    void MinimizeWindow();

    enum class FadeMode { None, In, OutHide, OutMinimize, OutClose };
    void StartWindowFade(FadeMode mode);
    void ApplyLayeredAlpha(BYTE alpha);
    void ClearLayeredStyle();
    void UpdateIdlePurgeStatusText();
    void UpdateScrollThumbFor(
        const winrt::Windows::UI::Xaml::Controls::ScrollViewer& sv,
        const winrt::Windows::UI::Xaml::Controls::Grid& track,
        const winrt::Windows::UI::Xaml::Controls::Border& thumb,
        const winrt::Windows::UI::Xaml::Media::TranslateTransform& tt);
    void UpdateCustomScrollThumb();
    void UpdateGameScrollThumb();
    void AttachDragHandlers(const winrt::Windows::UI::Xaml::UIElement& element);

    void RefreshMemorySnapshot(bool checkAutoPurge);
    void UpdateGaugeVisuals(double percent);
    void TriggerPurgeAsync(bool fromAuto);

    void AddProtectedGameExe(const std::wstring& exeNameOrPath);
    void RemoveProtectedGameExe(const std::wstring& exeName);
    void BrowseForGameExe();
    void RefreshGameProtectionUi(bool applyBoostNow, bool rebuildLists);
    void RebuildGameListsUi(const std::vector<RunningProcessInfo>& runningProcs);

    void InitTrayIcon();
    void UpdateTrayIcon(const MemorySnapshot& snap, bool forceIconRecreate);
    void RemoveTrayIcon();
    void ShowTrayContextMenu();

    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_hwndXamlIsland = nullptr;
    bool m_startMinimized = false;
    bool m_isElevated = false;
    bool m_xamlInitFailed = false;
    bool m_hydratingUi = false;
    bool m_trayAdded = false;
    bool m_closingForReal = false;
    ActiveView m_activeView = ActiveView::Main;
    bool m_isDraggingWindow = false;
    POINT m_dragStartCursor{};
    POINT m_dragStartWin{};
    FadeMode m_fadeMode = FadeMode::None;
    double m_fadeProgress = 0.0;
    std::atomic<bool> m_purgeRunning{false};

    UINT m_dpi = 96;
    uint32_t m_lastTrayPercent = 999;
    std::wstring m_lastTrayStyle;
    HICON m_currentTrayIcon = nullptr;
    HICON m_appIconLarge = nullptr;
    HICON m_appIconSmall = nullptr;

    double m_displayedPercent = 0.0;
    double m_targetPercent = 0.0;
    uint64_t m_lastPurgeTickMs = 0;
    uint64_t m_toastPurgeStartTick = 0;
    uint32_t m_gameBoostTickDivider = 0;

    AppConfig m_config;
    MemorySnapshot m_lastSnapshot;
    ProtectedGameStatus m_lastGameStatus;
    TrayIconRenderer m_trayRenderer;
    PurgeToastWindow m_toastWindow;
    std::unique_ptr<PurgeResult> m_pendingToastResult;

    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager m_xamlManager{nullptr};
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource m_xamlSource{nullptr};
    winrt::Windows::UI::Xaml::FrameworkElement m_root{nullptr};

    winrt::Windows::UI::Xaml::Media::Animation::Storyboard m_startupSloganSb{nullptr};
    winrt::Windows::UI::Xaml::Media::Animation::Storyboard m_purgePulseSb{nullptr};
    winrt::Windows::UI::Xaml::Media::Animation::Storyboard m_gearSpinInSb{nullptr};
    winrt::Windows::UI::Xaml::Media::Animation::Storyboard m_gearSpinOutSb{nullptr};
    winrt::Windows::UI::Xaml::Media::Animation::Storyboard m_openSettingsSb{nullptr};
    winrt::Windows::UI::Xaml::Media::Animation::Storyboard m_openMainSb{nullptr};
    winrt::Windows::UI::Xaml::Media::Animation::Storyboard m_openGamesSb{nullptr};

    winrt::Windows::UI::Xaml::Controls::Grid m_titleBarDragArea{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid m_settingsHeaderDragArea{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid m_gameHeaderDragArea{nullptr};
    winrt::Windows::UI::Xaml::Controls::Viewbox m_logoFallbackBadge{nullptr};
    winrt::Windows::UI::Xaml::Controls::Image m_logoSvgImage{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_winMinimizeBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_winCloseBtn{nullptr};

    winrt::Windows::UI::Xaml::Controls::Grid m_mainView{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid m_settingsView{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid m_gameView{nullptr};
    winrt::Windows::UI::Xaml::Shapes::Path m_usageArcPath{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_usagePercentText{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_usageRatioText{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_inUseText{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_availableText{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_cachedText{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_gameShieldBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_gameShieldTitleText{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_gameShieldSubText{nullptr};
    winrt::Windows::UI::Xaml::Controls::Border m_gameShieldBadgeBorder{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_gameShieldBadgeText{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_purgeBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_purgeBtnText{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_purgeStatusText{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_settingsBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_minTrayBtn{nullptr};

    winrt::Windows::UI::Xaml::Controls::Button m_gameBackBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_boostGamesNowBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::ScrollViewer m_gameScrollViewer{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid m_gameScrollTrack{nullptr};
    winrt::Windows::UI::Xaml::Controls::Border m_gameScrollThumb{nullptr};
    winrt::Windows::UI::Xaml::Media::TranslateTransform m_gameScrollThumbTransform{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglMaximizeGame{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_browseGameBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_refreshProcsBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_gameActionStatusText{nullptr};
    winrt::Windows::UI::Xaml::Controls::Border m_noGamesEmptyCard{nullptr};
    winrt::Windows::UI::Xaml::Controls::StackPanel m_protectedGamesList{nullptr};
    winrt::Windows::UI::Xaml::Controls::StackPanel m_runningProcessesList{nullptr};

    winrt::Windows::UI::Xaml::Controls::Button m_backBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::ScrollViewer m_settingsScrollViewer{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid m_customScrollTrack{nullptr};
    winrt::Windows::UI::Xaml::Controls::Border m_customScrollThumb{nullptr};
    winrt::Windows::UI::Xaml::Media::TranslateTransform m_scrollThumbTransform{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglAutoStart{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglStartMinimized{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglMinToTray{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglCloseToTray{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_trayModeLogoBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_trayModePercentBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglAlwaysOnTop{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglWorkingSets{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglSystemCache{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglStandbyList{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglModifiedList{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglAutoPurge{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_thresholdPrevBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_thresholdNextBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_thresholdValueText{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_intervalPrevBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_intervalNextBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock m_intervalValueText{nullptr};
    winrt::Windows::UI::Xaml::Controls::ToggleSwitch m_tglNotifications{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid m_settingsElevateRow{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_settingsElevateBtn{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button m_supportDevBtn{nullptr};
};

}
