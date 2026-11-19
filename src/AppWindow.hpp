#pragma once

#include "ConfigManager.hpp"
#include "MemoryEngine.hpp"
#include "PurgeToastWindow.hpp"
#include "TrayIconRenderer.hpp"
#include "VectorIcons.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vram {

constexpr UINT WM_APP_SHOW_WINDOW = WM_APP + 1;
constexpr UINT WM_APP_TRAY_ICON   = WM_APP + 2;
constexpr UINT WM_APP_PURGE_DONE  = WM_APP + 3;

/// Frameless Direct2D/DirectWrite native desktop window with zero XAML/WinRT UI dependencies.
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
    enum class ActiveView : size_t {
        Main = 0,
        Settings = 1,
        Games = 2
    };

    enum class FadeMode {
        None,
        In,
        OutHide,
        OutMinimize,
        OutClose
    };

    enum class ControlId {
        None = 0,
        WinMinimize,
        WinClose,
        GameShieldCard,
        PurgeBtn,
        SettingsBtn,
        MinTrayBtn,
        GameBackBtn,
        BoostGamesBtn,
        ToggleMaximizeGame,
        BrowseGameBtn,
        RefreshProcsBtn,
        RemoveProtectedGame,
        AddRunningProcess,
        BackBtn,
        ToggleAutoStart,
        ToggleStartMinimized,
        ToggleMinToTray,
        ToggleCloseToTray,
        TrayLogoBtn,
        TrayPercentBtn,
        ToggleAlwaysOnTop,
        ToggleWorkingSets,
        ToggleSystemCache,
        ToggleStandbyList,
        ToggleModifiedList,
        ToggleAutoPurge,
        ThresholdPrevBtn,
        ThresholdNextBtn,
        IntervalPrevBtn,
        IntervalNextBtn,
        ToggleNotifications,
        ElevateBtn,
        SupportDevBtn,
        ScrollThumb
    };

    enum class ToggleId : size_t {
        MaximizeGame = 0,
        AutoStart,
        StartMinimized,
        MinimizeToTray,
        CloseToTray,
        AlwaysOnTop,
        WorkingSets,
        SystemCache,
        StandbyList,
        ModifiedList,
        AutoPurge,
        Notifications,
        Count
    };

    struct HitControl {
        ControlId id = ControlId::None;
        int arg = -1;
        D2D1_RECT_F rect{};
        std::wstring tooltip;
    };

    struct ControlTarget {
        ControlId id = ControlId::None;
        int arg = -1;
        bool operator==(const ControlTarget& o) const { return id == o.id && arg == o.arg; }
        bool operator!=(const ControlTarget& o) const { return !(*this == o); }
        bool valid() const { return id != ControlId::None; }
    };

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnDestroy();
    void OnPaint();
    void OnSize(UINT width, UINT height);
    void OnDpiChanged(UINT newDpi, const RECT* suggestedRect);
    void OnTimer(UINT_PTR timerId);
    void OnMouseMove(int px, int py);
    void OnMouseLeave();
    void OnLButtonDown(int px, int py);
    void OnLButtonUp(int px, int py);
    void OnMouseWheel(short wheelDelta);
    void OnKeyDown(WPARAM vk);
    void OnTrayMessage(LPARAM lParam);
    void OnPurgeFinished(PurgeResult* result);

    bool InitializeDeviceIndependentGraphics();
    bool EnsureRenderTarget(int widthPx, int heightPx);
    void PaintToHdc(HDC dstHdc, int widthPx, int heightPx);
    void DiscardRenderTarget();
    void LoadLogoBitmap();
    void RenderD2D(ID2D1RenderTarget* rt);
    void RenderGdiFallback(HDC hdc, const RECT& clientRc);

    void DrawHeader(ID2D1RenderTarget* rt, float w);
    void DrawMainView(ID2D1RenderTarget* rt, float w, float h, float slideX, float opacity);
    void DrawGameView(ID2D1RenderTarget* rt, float w, float h, float slideX, float opacity);
    void DrawSettingsView(ID2D1RenderTarget* rt, float w, float h, float slideX, float opacity);

    void DrawToggleRow(ID2D1RenderTarget* rt,
                       float x, float y, float w, float h,
                       VectorIconId icon, bool hasIcon,
                       const wchar_t* title, const wchar_t* subtitle,
                       ToggleId toggleId, ControlId controlId, bool checked);
    void DrawToggleSwitch(ID2D1RenderTarget* rt, const D2D1_RECT_F& rect, float animT, bool hovered);
    void DrawIconButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc,
                        ControlId id, VectorIconId icon, float iconDip,
                        D2D1_COLOR_F fg, D2D1_COLOR_F hoverBg, D2D1_COLOR_F pressBg,
                        float strokeW = 1.9f, float rotDeg = 0.0f, const wchar_t* tooltip = nullptr);
    void DrawPrimaryButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc,
                           ControlId id, const wchar_t* text, float fontSize,
                           float cornerRadius, bool enabled,
                           VectorIconId prefixIcon = VectorIconId::Count,
                           const wchar_t* tooltip = nullptr);
    void DrawCustomScrollbar(ID2D1RenderTarget* rt, float x, float topY, float viewH,
                             float contentH, float scrollOffset);
    void DrawTooltipOverlay(ID2D1RenderTarget* rt, float w, float h);

    void RegisterHit(ControlId id, const D2D1_RECT_F& rc, const wchar_t* tooltip = nullptr, int arg = -1);
    ControlTarget HitTest(float dipX, float dipY, std::wstring* outTooltip = nullptr, D2D1_RECT_F* outRect = nullptr) const;
    void ActivateControl(const ControlTarget& target);

    void StartUiAnimationLoop();
    void SyncToggleTargetsFromConfig(bool immediate);
    bool AdvanceAnimations();

    void ApplyDwmTheme();
    void ApplyAlwaysOnTop();
    void SaveAndApplyConfig();
    void SwitchToView(ActiveView view);
    void ShowMainWindow();
    void HideToTray();
    void MinimizeWindow();

    void StartWindowFade(FadeMode mode);
    void ApplyLayeredAlpha(BYTE alpha);
    void ClearLayeredStyle();
    void UpdateIdlePurgeStatusText();
    void ScrollBy(float deltaDip);
    void ClampActiveScroll();

    void RefreshMemorySnapshot(bool checkAutoPurge);
    void TriggerPurgeAsync(bool fromAuto);

    void AddProtectedGameExe(const std::wstring& exeNameOrPath);
    void RemoveProtectedGameExe(const std::wstring& exeName);
    void BrowseForGameExe();
    void RefreshGameProtectionUi(bool applyBoostNow, bool rebuildLists);

    void InitTrayIcon();
    void UpdateTrayIcon(const MemorySnapshot& snap, bool forceIconRecreate);
    void RemoveTrayIcon();
    void ShowTrayContextMenu();

    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    bool m_startMinimized = false;
    bool m_isElevated = false;
    bool m_trayAdded = false;
    bool m_closingForReal = false;
    ActiveView m_activeView = ActiveView::Main;

    bool m_isDraggingWindow = false;
    POINT m_dragStartCursor{};
    POINT m_dragStartWin{};
    bool m_trackingMouseLeave = false;
    bool m_draggingScrollThumb = false;
    float m_scrollDragStartDipY = 0.0f;
    float m_scrollDragStartOffset = 0.0f;

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
    float m_sloganAnimT = 0.0f;
    float m_sloganAnimTarget = 1.0f;
    float m_purgePulsePhase = 0.0f;
    float m_gearRotDeg = 0.0f;
    float m_viewTransT = 1.0f;
    int m_viewSlideDir = 1;
    std::array<float, static_cast<size_t>(ToggleId::Count)> m_togglePos{};
    bool m_animTimerActive = false;

    uint64_t m_lastPurgeTickMs = 0;
    uint64_t m_toastPurgeStartTick = 0;
    uint32_t m_gameBoostTickDivider = 0;

    std::wstring m_purgeStatusMsg = L"Auto Purge: OFF";
    bool m_purgeStatusAccent = false;
    std::wstring m_gameActionMsg = L"Selected games are never trimmed during Purge.";
    bool m_gameActionAccent = false;

    std::array<float, 3> m_scrollOffset{};
    std::array<float, 3> m_scrollMaxOffset{};
    std::array<float, 3> m_scrollContentHeight{};
    float m_scrollViewHeight = 410.0f;
    D2D1_RECT_F m_activeScrollTrack{};

    ControlTarget m_hoverControl{};
    ControlTarget m_pressedControl{};
    uint64_t m_hoverStartTick = 0;
    bool m_tooltipVisible = false;
    std::wstring m_hoverTooltipText;
    D2D1_RECT_F m_hoverTooltipAnchor{};

    std::vector<HitControl> m_hitControls;
    std::vector<std::wstring> m_shownRunningProcs;
    std::vector<RunningProcessInfo> m_cachedRunningProcs;

    AppConfig m_config;
    MemorySnapshot m_lastSnapshot;
    ProtectedGameStatus m_lastGameStatus;
    TrayIconRenderer m_trayRenderer;
    PurgeToastWindow m_toastWindow;
    std::unique_ptr<PurgeResult> m_pendingToastResult;

    // Direct2D / DirectWrite / Offscreen DIB resources
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_rt;
    HDC m_memDc = nullptr;
    HBITMAP m_memBmp = nullptr;
    HGDIOBJ m_oldMemBmp = nullptr;
    int m_memW = 0;
    int m_memH = 0;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_logoBitmap;
    uint32_t m_logoBmpW = 0;
    uint32_t m_logoBmpH = 0;
    std::vector<uint8_t> m_logoPbgra;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> m_roundCapStroke;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtPercent;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtHeaderTitle;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBtnLarge;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtMetricVal;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtMetricLbl;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtRowTitle;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBody;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtBodySemi;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtSub;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtSection;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtSmallCenter;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmtSmallBoldCenter;

    VectorIconCache m_icons;
};

}
