#include "AppWindow.hpp"
#include "../resources/resource.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace winrt_wux  = winrt::Windows::UI::Xaml;
namespace winrt_ctrl = winrt::Windows::UI::Xaml::Controls;
namespace winrt_med  = winrt::Windows::UI::Xaml::Media;
namespace winrt_anim = winrt::Windows::UI::Xaml::Media::Animation;
namespace winrt_img  = winrt::Windows::UI::Xaml::Media::Imaging;
namespace winrt_shp  = winrt::Windows::UI::Xaml::Shapes;

namespace vram {

namespace {

constexpr wchar_t kWindowClassName[] = L"VRam_V1_HostWindow";
constexpr int kClientDipWidth  = 368;
constexpr int kClientDipHeight = 488;

constexpr UINT_PTR IDT_POLL_MEMORY  = 1001;
constexpr UINT_PTR IDT_ARC_ANIM     = 1002;
constexpr UINT_PTR IDT_WINDOW_FADE  = 1003;
constexpr UINT_PTR IDT_STATUS_RESET = 1004;
constexpr UINT     kPollIntervalMs  = 2000;
constexpr UINT     kArcAnimMs       = 16;
constexpr UINT     kFadeTimerMs     = 12;
constexpr UINT     kStatusResetMs   = 4000;

constexpr UINT kTrayIconId = 1;
constexpr UINT IDM_TRAY_SHOW     = 40001;
constexpr UINT IDM_TRAY_PURGE    = 40002;
constexpr UINT IDM_TRAY_SETTINGS = 40003;
constexpr UINT IDM_TRAY_EXIT     = 40004;

constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmRoundCorner            = 2;
constexpr DWORD kDwmBorderColor            = 34;

constexpr std::array<int, 5> kThresholds = {70, 75, 80, 85, 90};
constexpr std::array<int, 5> kIntervals  = {0, 5, 15, 30, 60};

winrt::Windows::UI::Color RgbColor(uint8_t r, uint8_t g, uint8_t b) {
    return winrt::Windows::UI::ColorHelper::FromArgb(255, r, g, b);
}

std::wstring LoadUtf8ResourceAsWide(HINSTANCE hInst, UINT resId) {
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hMem = LoadResource(hInst, hRes);
    if (!hMem) return {};
    const DWORD size = SizeofResource(hInst, hRes);
    const char* data = static_cast<const char*>(LockResource(hMem));
    if (!data || size == 0) return {};

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), nullptr, 0);
    if (wideLen <= 0) return {};
    std::wstring out(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), out.data(), wideLen);
    return out;
}

template <size_t N>
int StepOption(const std::array<int, N>& values, int current, int delta) {
    int idx = 0;
    for (size_t i = 0; i < N; ++i) {
        if (values[i] == current) {
            idx = static_cast<int>(i);
            break;
        }
    }
    const int next = (idx + delta + static_cast<int>(N)) % static_cast<int>(N);
    return values[next];
}

}

AppWindow::AppWindow(HINSTANCE hInst, bool startMinimized)
    : m_hInst(hInst)
    , m_startMinimized(startMinimized)
    , m_isElevated(MemoryEngine::IsElevated()) {
    m_config = ConfigManager::Load();
    m_lastPurgeTickMs = GetTickCount64();
}

AppWindow::~AppWindow() {
    if (m_appIconLarge) DestroyIcon(m_appIconLarge);
    if (m_appIconSmall) DestroyIcon(m_appIconSmall);
    if (m_currentTrayIcon) DestroyIcon(m_currentTrayIcon);
}

bool AppWindow::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = m_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(255, 255, 255));
    wc.lpszClassName = kWindowClassName;

    m_dpi = GetDpiForSystem();
    m_appIconLarge = static_cast<HICON>(LoadImageW(
        m_hInst, MAKEINTRESOURCEW(IDI_VRAM_APP), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, m_dpi),
        GetSystemMetricsForDpi(SM_CYICON, m_dpi),
        LR_DEFAULTCOLOR
    ));
    m_appIconSmall = static_cast<HICON>(LoadImageW(
        m_hInst, MAKEINTRESOURCEW(IDI_VRAM_APP), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, m_dpi),
        GetSystemMetricsForDpi(SM_CYSMICON, m_dpi),
        LR_DEFAULTCOLOR
    ));
    wc.hIcon = m_appIconLarge;
    wc.hIconSm = m_appIconSmall;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;

    const int winW = MulDiv(kClientDipWidth, m_dpi, 96);
    const int winH = MulDiv(kClientDipHeight, m_dpi, 96);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int posX = workArea.left + ((workArea.right - workArea.left) - winW) / 2;
    const int posY = workArea.top + ((workArea.bottom - workArea.top) - winH) / 2;

    m_hwnd = CreateWindowExW(
        exStyle,
        kWindowClassName,
        L"VRam V1.0",
        style,
        posX, posY, winW, winH,
        nullptr, nullptr, m_hInst, this
    );
    if (!m_hwnd) {
        return false;
    }

    ApplyDwmTheme();
    ApplyAlwaysOnTop();

    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    const bool shouldHideOnLaunch = m_startMinimized || m_config.startMinimized;
    if (shouldHideOnLaunch) {
        ShowWindow(m_hwnd, SW_HIDE);
    } else {
        StartWindowFade(FadeMode::In);
        if (m_startupSloganSb) {
            m_startupSloganSb.Begin();
        }
    }

    return true;
}

bool AppWindow::PreTranslateMessage(MSG* msg) {
    if (m_xamlSource) {
        if (auto native2 = m_xamlSource.try_as<IDesktopWindowXamlSourceNative2>()) {
            BOOL processed = FALSE;
            if (SUCCEEDED(native2->PreTranslateMessage(msg, &processed)) && processed) {
                return true;
            }
        }
    }
    return false;
}

LRESULT CALLBACK AppWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<AppWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->WndProc(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT AppWindow::WndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;

    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            return 0;
        }
        break;

    case WM_NCACTIVATE:
        return TRUE;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(m_hwnd, &pt);
        if (pt.y >= 0 && pt.y < MulDiv(48, m_dpi, 96)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_SIZE:
        OnSize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            if (m_config.minimizeToTray) {
                HideToTray();
            } else {
                MinimizeWindow();
            }
            return 0;
        }
        if ((wParam & 0xFFF0) == SC_RESTORE && IsIconic(m_hwnd)) {
            DefWindowProcW(m_hwnd, msg, wParam, lParam);
            StartWindowFade(FadeMode::In);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (!m_closingForReal && m_config.closeToTray) {
            HideToTray();
            return 0;
        }
        if (!m_closingForReal && IsWindowVisible(m_hwnd)) {
            StartWindowFade(FadeMode::OutClose);
            return 0;
        }
        DestroyWindow(m_hwnd);
        return 0;

    case WM_DESTROY:
        OnDestroy();
        PostQuitMessage(0);
        return 0;

    case WM_TIMER:
        OnTimer(static_cast<UINT_PTR>(wParam));
        return 0;

    case WM_APP_SHOW_WINDOW:
        ShowMainWindow();
        return 0;

    case WM_APP_TRAY_ICON:
        OnTrayMessage(lParam);
        return 0;

    case WM_APP_PURGE_DONE:
        OnPurgeFinished(reinterpret_cast<PurgeResult*>(lParam));
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TRAY_SHOW:
            if (IsWindowVisible(m_hwnd)) HideToTray();
            else ShowMainWindow();
            return 0;
        case IDM_TRAY_PURGE:
            TriggerPurgeAsync(false);
            return 0;
        case IDM_TRAY_SETTINGS:
            ShowMainWindow();
            SwitchToView(true);
            return 0;
        case IDM_TRAY_EXIT:
            m_closingForReal = true;
            DestroyWindow(m_hwnd);
            return 0;
        }
        break;
    }

    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

void AppWindow::OnCreate() {
    m_dpi = GetDpiForWindow(m_hwnd);

    m_xamlManager = winrt_wux::Hosting::WindowsXamlManager::InitializeForCurrentThread();
    m_xamlSource = winrt_wux::Hosting::DesktopWindowXamlSource();

    auto native = m_xamlSource.as<IDesktopWindowXamlSourceNative>();
    winrt::check_hresult(native->AttachToWindow(m_hwnd));
    winrt::check_hresult(native->get_WindowHandle(&m_hwndXamlIsland));

    const std::wstring xamlXml = LoadUtf8ResourceAsWide(m_hInst, IDR_XAML_LAYOUT);
    auto parsed = winrt_wux::Markup::XamlReader::Load(winrt::hstring(xamlXml));
    m_root = parsed.as<winrt_wux::FrameworkElement>();
    m_xamlSource.Content(m_root);

    ResizeXamlIsland();
    BindXamlControls();
    LoadLogoSvgIntoUi();
    HydrateSettingsUi();

    InitTrayIcon();
    RefreshMemorySnapshot(false);
    m_displayedPercent = m_targetPercent;
    UpdateGaugeVisuals(m_displayedPercent);

    SetTimer(m_hwnd, IDT_POLL_MEMORY, kPollIntervalMs, nullptr);
}

void AppWindow::OnDestroy() {
    KillTimer(m_hwnd, IDT_POLL_MEMORY);
    KillTimer(m_hwnd, IDT_ARC_ANIM);
    KillTimer(m_hwnd, IDT_WINDOW_FADE);
    KillTimer(m_hwnd, IDT_STATUS_RESET);
    RemoveTrayIcon();

    if (m_xamlSource) {
        m_xamlSource.Close();
        m_xamlSource = nullptr;
    }
    if (m_xamlManager) {
        m_xamlManager.Close();
        m_xamlManager = nullptr;
    }
}

void AppWindow::OnSize(UINT, UINT) {
    ResizeXamlIsland();
}

void AppWindow::OnDpiChanged(UINT newDpi, const RECT* suggestedRect) {
    m_dpi = newDpi;
    if (suggestedRect) {
        SetWindowPos(m_hwnd, nullptr,
                     suggestedRect->left, suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ResizeXamlIsland();
    UpdateTrayIcon(m_lastSnapshot, true);
}

void AppWindow::OnTimer(UINT_PTR timerId) {
    if (timerId == IDT_POLL_MEMORY) {
        RefreshMemorySnapshot(true);
    } else if (timerId == IDT_ARC_ANIM) {
        const double diff = m_targetPercent - m_displayedPercent;
        if (std::fabs(diff) <= 0.25) {
            m_displayedPercent = m_targetPercent;
            UpdateGaugeVisuals(m_displayedPercent);
            KillTimer(m_hwnd, IDT_ARC_ANIM);
        } else {
            m_displayedPercent += diff * 0.22;
            UpdateGaugeVisuals(m_displayedPercent);
        }
    } else if (timerId == IDT_WINDOW_FADE) {
        if (m_fadeMode == FadeMode::In) {
            m_fadeProgress = std::min(1.0, m_fadeProgress + 0.085);
            const double eased = 1.0 - std::pow(1.0 - m_fadeProgress, 3.0);
            ApplyLayeredAlpha(static_cast<BYTE>(std::lround(eased * 255.0)));
            if (m_fadeProgress >= 1.0) {
                KillTimer(m_hwnd, IDT_WINDOW_FADE);
                m_fadeMode = FadeMode::None;
                ClearLayeredStyle();
            }
        } else if (m_fadeMode == FadeMode::OutHide ||
                   m_fadeMode == FadeMode::OutMinimize ||
                   m_fadeMode == FadeMode::OutClose) {
            m_fadeProgress = std::max(0.0, m_fadeProgress - 0.10);
            const double eased = m_fadeProgress * m_fadeProgress;
            ApplyLayeredAlpha(static_cast<BYTE>(std::lround(eased * 255.0)));
            if (m_fadeProgress <= 0.0) {
                KillTimer(m_hwnd, IDT_WINDOW_FADE);
                const FadeMode doneMode = m_fadeMode;
                m_fadeMode = FadeMode::None;
                if (doneMode == FadeMode::OutHide) {
                    ShowWindow(m_hwnd, SW_HIDE);
                    ClearLayeredStyle();
                } else if (doneMode == FadeMode::OutMinimize) {
                    ShowWindow(m_hwnd, SW_MINIMIZE);
                    ClearLayeredStyle();
                } else if (doneMode == FadeMode::OutClose) {
                    m_closingForReal = true;
                    DestroyWindow(m_hwnd);
                }
            }
        } else {
            KillTimer(m_hwnd, IDT_WINDOW_FADE);
            ClearLayeredStyle();
        }
    } else if (timerId == IDT_STATUS_RESET) {
        KillTimer(m_hwnd, IDT_STATUS_RESET);
        if (!m_purgeRunning.load()) {
            UpdateIdlePurgeStatusText();
        }
    }
}

void AppWindow::OnTrayMessage(LPARAM lParam) {
    const UINT mouseMsg = LOWORD(lParam);
    if (mouseMsg == WM_LBUTTONUP || mouseMsg == WM_LBUTTONDBLCLK) {
        if (IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd)) {
            HideToTray();
        } else {
            ShowMainWindow();
        }
    } else if (mouseMsg == WM_RBUTTONUP || mouseMsg == WM_CONTEXTMENU) {
        ShowTrayContextMenu();
    }
}

void AppWindow::OnPurgeFinished(PurgeResult* rawResult) {
    std::unique_ptr<PurgeResult> res(rawResult);
    m_purgeRunning = false;
    m_lastPurgeTickMs = GetTickCount64();

    if (m_purgePulseSb) {
        m_purgePulseSb.Stop();
    }
    if (m_purgeBtn) m_purgeBtn.IsEnabled(true);
    if (m_purgeBtnText) m_purgeBtnText.Text(L"Purge Memory");

    if (res && m_purgeStatusText) {
        wchar_t msg[160]{};
        if (res->freedBytes > 0) {
            std::wstring freedStr = MemoryEngine::FormatBytes(res->freedBytes);
            std::swprintf(msg, 160, L"Freed %s (%u%% -> %u%%)",
                          freedStr.c_str(), res->beforePercent, res->afterPercent);
            if (!m_isElevated && res->privilegeLimited) {
                wcscat_s(msg, L" | Elevate for standby");
            }
        } else if (!m_isElevated && res->privilegeLimited) {
            std::swprintf(msg, 160, L"Trimmed %u processes | Elevate for standby",
                          res->trimmedProcesses);
        } else {
            std::swprintf(msg, 160, L"Already compact (%u%%)", res->afterPercent);
        }

        m_purgeStatusText.Text(winrt::hstring(msg));
        m_purgeStatusText.Foreground(winrt_med::SolidColorBrush(
            res->freedBytes > 0 ? RgbColor(109, 40, 217) : RgbColor(113, 113, 122)
        ));

        if (m_config.showNotifications && !IsWindowVisible(m_hwnd)) {
            ShowTrayBalloon(L"VRam V1.0", msg);
        }
        SetTimer(m_hwnd, IDT_STATUS_RESET, kStatusResetMs, nullptr);
    }

    RefreshMemorySnapshot(false);
}

void AppWindow::ApplyDwmTheme() {
    DwmSetWindowAttribute(m_hwnd, kDwmWindowCornerPreference, &kDwmRoundCorner, sizeof(kDwmRoundCorner));
    COLORREF border = RGB(224, 224, 232);
    DwmSetWindowAttribute(m_hwnd, kDwmBorderColor, &border, sizeof(border));
    MARGINS shadowMargins{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &shadowMargins);
}

void AppWindow::ApplyAlwaysOnTop() {
    SetWindowPos(m_hwnd,
                 m_config.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void AppWindow::ResizeXamlIsland() {
    if (!m_hwndXamlIsland) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    SetWindowPos(m_hwndXamlIsland, nullptr,
                 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_SHOWWINDOW);
}

void AppWindow::BindXamlControls() {
    auto find = [&](const wchar_t* name) {
        return m_root.FindName(winrt::hstring(name));
    };

    auto resources = m_root.Resources();
    auto findSb = [&](const wchar_t* name) -> winrt_anim::Storyboard {
        if (auto obj = find(name)) {
            return obj.try_as<winrt_anim::Storyboard>();
        }
        if (resources && resources.HasKey(winrt::box_value(name))) {
            return resources.Lookup(winrt::box_value(name)).try_as<winrt_anim::Storyboard>();
        }
        return nullptr;
    };

    m_startupSloganSb = findSb(L"StartupSloganStoryboard");
    m_purgePulseSb    = findSb(L"PurgePulseStoryboard");
    m_gearSpinInSb    = findSb(L"GearSpinInStoryboard");
    m_gearSpinOutSb   = findSb(L"GearSpinOutStoryboard");
    m_openSettingsSb  = findSb(L"OpenSettingsStoryboard");
    m_openMainSb      = findSb(L"OpenMainStoryboard");

    m_titleBarDragArea       = find(L"TitleBarDragArea").as<winrt_ctrl::Grid>();
    m_settingsHeaderDragArea = find(L"SettingsHeaderDragArea").as<winrt_ctrl::Grid>();
    m_logoFallbackBadge      = find(L"LogoFallbackBadge").as<winrt_ctrl::Viewbox>();
    m_logoSvgImage        = find(L"LogoSvgImage").as<winrt_ctrl::Image>();
    m_winMinimizeBtn      = find(L"WinMinimizeButton").as<winrt_ctrl::Button>();
    m_winCloseBtn         = find(L"WinCloseButton").as<winrt_ctrl::Button>();

    m_mainView         = find(L"MainView").as<winrt_ctrl::Grid>();
    m_settingsView     = find(L"SettingsView").as<winrt_ctrl::Grid>();
    m_usageArcPath     = find(L"UsageArcPath").as<winrt_shp::Path>();
    m_usagePercentText = find(L"UsagePercentText").as<winrt_ctrl::TextBlock>();
    m_usageRatioText   = find(L"UsageRatioText").as<winrt_ctrl::TextBlock>();
    m_inUseText        = find(L"InUseText").as<winrt_ctrl::TextBlock>();
    m_availableText    = find(L"AvailableText").as<winrt_ctrl::TextBlock>();
    m_cachedText       = find(L"CachedText").as<winrt_ctrl::TextBlock>();
    m_purgeBtn         = find(L"PurgeButton").as<winrt_ctrl::Button>();
    m_purgeBtnText     = find(L"PurgeButtonText").as<winrt_ctrl::TextBlock>();
    m_purgeStatusText  = find(L"PurgeStatusText").as<winrt_ctrl::TextBlock>();
    m_settingsBtn      = find(L"SettingsButton").as<winrt_ctrl::Button>();
    m_minTrayBtn       = find(L"MinimizeTrayButton").as<winrt_ctrl::Button>();

    m_backBtn              = find(L"BackButton").as<winrt_ctrl::Button>();
    m_settingsScrollViewer = find(L"SettingsScrollViewer").as<winrt_ctrl::ScrollViewer>();
    m_customScrollTrack    = find(L"CustomScrollTrack").as<winrt_ctrl::Grid>();
    m_customScrollThumb    = find(L"CustomScrollThumb").as<winrt_ctrl::Border>();
    m_scrollThumbTransform = find(L"ScrollThumbTransform").as<winrt_med::TranslateTransform>();
    m_tglAutoStart       = find(L"ToggleAutoStart").as<winrt_ctrl::ToggleSwitch>();
    m_tglStartMinimized  = find(L"ToggleStartMinimized").as<winrt_ctrl::ToggleSwitch>();
    m_tglMinToTray       = find(L"ToggleMinimizeToTray").as<winrt_ctrl::ToggleSwitch>();
    m_tglCloseToTray     = find(L"ToggleCloseToTray").as<winrt_ctrl::ToggleSwitch>();
    m_trayModeLogoBtn    = find(L"TrayModeLogoBtn").as<winrt_ctrl::Button>();
    m_trayModePercentBtn = find(L"TrayModePercentBtn").as<winrt_ctrl::Button>();
    m_tglAlwaysOnTop     = find(L"ToggleAlwaysOnTop").as<winrt_ctrl::ToggleSwitch>();
    m_tglWorkingSets     = find(L"ToggleWorkingSets").as<winrt_ctrl::ToggleSwitch>();
    m_tglSystemCache     = find(L"ToggleSystemCache").as<winrt_ctrl::ToggleSwitch>();
    m_tglStandbyList     = find(L"ToggleStandbyList").as<winrt_ctrl::ToggleSwitch>();
    m_tglModifiedList    = find(L"ToggleModifiedList").as<winrt_ctrl::ToggleSwitch>();
    m_tglAutoPurge       = find(L"ToggleAutoPurge").as<winrt_ctrl::ToggleSwitch>();
    m_thresholdPrevBtn   = find(L"ThresholdPrevBtn").as<winrt_ctrl::Button>();
    m_thresholdNextBtn   = find(L"ThresholdNextBtn").as<winrt_ctrl::Button>();
    m_thresholdValueText = find(L"ThresholdValueText").as<winrt_ctrl::TextBlock>();
    m_intervalPrevBtn    = find(L"IntervalPrevBtn").as<winrt_ctrl::Button>();
    m_intervalNextBtn    = find(L"IntervalNextBtn").as<winrt_ctrl::Button>();
    m_intervalValueText  = find(L"IntervalValueText").as<winrt_ctrl::TextBlock>();
    m_tglNotifications   = find(L"ToggleNotifications").as<winrt_ctrl::ToggleSwitch>();
    m_settingsElevateRow = find(L"SettingsElevateRow").as<winrt_ctrl::Grid>();
    m_settingsElevateBtn = find(L"SettingsElevateButton").as<winrt_ctrl::Button>();
    m_supportDevBtn      = find(L"SupportDevButton").as<winrt_ctrl::Button>();

    AttachDragHandlers(m_titleBarDragArea);
    AttachDragHandlers(m_settingsHeaderDragArea);
    AttachDragHandlers(m_mainView);

    m_winMinimizeBtn.Click([this](auto&&, auto&&) {
        if (m_config.minimizeToTray) {
            HideToTray();
        } else {
            MinimizeWindow();
        }
    });

    m_winCloseBtn.Click([this](auto&&, auto&&) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    });

    m_settingsElevateRow.Visibility(m_isElevated ? winrt_wux::Visibility::Collapsed : winrt_wux::Visibility::Visible);

    m_settingsElevateBtn.Click([this](winrt::Windows::Foundation::IInspectable const&, winrt_wux::RoutedEventArgs const&) {
        if (m_isElevated) return;
        if (MemoryEngine::RestartElevated(false)) {
            m_closingForReal = true;
            DestroyWindow(m_hwnd);
        }
    });

    m_supportDevBtn.Click([](auto&&, auto&&) {
        ShellExecuteW(nullptr, L"open", L"https://sociabuzz.com/idnvinnmalay", nullptr, nullptr, SW_SHOWNORMAL);
    });

    m_settingsScrollViewer.ViewChanged([this](auto&&, auto&&) {
        UpdateCustomScrollThumb();
    });
    m_settingsScrollViewer.SizeChanged([this](auto&&, auto&&) {
        UpdateCustomScrollThumb();
    });
    m_customScrollTrack.SizeChanged([this](auto&&, auto&&) {
        UpdateCustomScrollThumb();
    });

    m_purgeBtn.Click([this](auto&&, auto&&) {
        TriggerPurgeAsync(false);
    });

    m_settingsBtn.Click([this](auto&&, auto&&) {
        SwitchToView(true);
    });
    m_settingsBtn.PointerEntered([this](auto&&, auto&&) {
        if (m_gearSpinInSb) m_gearSpinInSb.Begin();
    });
    m_settingsBtn.PointerExited([this](auto&&, auto&&) {
        if (m_gearSpinOutSb) m_gearSpinOutSb.Begin();
    });

    m_backBtn.Click([this](auto&&, auto&&) {
        SwitchToView(false);
    });

    m_minTrayBtn.Click([this](auto&&, auto&&) {
        HideToTray();
    });

    auto onToggleChanged = [this](auto&&, auto&&) {
        if (!m_hydratingUi) SaveAndApplyConfig();
    };

    m_tglAutoStart.Toggled(onToggleChanged);
    m_tglStartMinimized.Toggled(onToggleChanged);
    m_tglMinToTray.Toggled(onToggleChanged);
    m_tglCloseToTray.Toggled(onToggleChanged);
    m_tglAlwaysOnTop.Toggled(onToggleChanged);
    m_tglWorkingSets.Toggled(onToggleChanged);
    m_tglSystemCache.Toggled(onToggleChanged);
    m_tglStandbyList.Toggled(onToggleChanged);
    m_tglModifiedList.Toggled(onToggleChanged);
    m_tglAutoPurge.Toggled(onToggleChanged);
    m_tglNotifications.Toggled(onToggleChanged);

    m_trayModeLogoBtn.Click([this](auto&&, auto&&) {
        m_config.trayStyle = L"logo";
        UpdateCustomOptionPills();
        SaveAndApplyConfig();
    });
    m_trayModePercentBtn.Click([this](auto&&, auto&&) {
        m_config.trayStyle = L"percent";
        UpdateCustomOptionPills();
        SaveAndApplyConfig();
    });

    m_thresholdPrevBtn.Click([this](auto&&, auto&&) {
        m_config.autoPurgeThreshold = StepOption(kThresholds, m_config.autoPurgeThreshold, -1);
        UpdateCustomOptionPills();
        SaveAndApplyConfig();
    });
    m_thresholdNextBtn.Click([this](auto&&, auto&&) {
        m_config.autoPurgeThreshold = StepOption(kThresholds, m_config.autoPurgeThreshold, +1);
        UpdateCustomOptionPills();
        SaveAndApplyConfig();
    });

    m_intervalPrevBtn.Click([this](auto&&, auto&&) {
        m_config.autoPurgeIntervalMinutes = StepOption(kIntervals, m_config.autoPurgeIntervalMinutes, -1);
        UpdateCustomOptionPills();
        SaveAndApplyConfig();
    });
    m_intervalNextBtn.Click([this](auto&&, auto&&) {
        m_config.autoPurgeIntervalMinutes = StepOption(kIntervals, m_config.autoPurgeIntervalMinutes, +1);
        UpdateCustomOptionPills();
        SaveAndApplyConfig();
    });
}

void AppWindow::AttachDragHandlers(const winrt_wux::UIElement& element) {
    if (!element) return;

    element.PointerPressed([this, element](auto&&, winrt_wux::Input::PointerRoutedEventArgs const& args) {
        auto curNode = args.OriginalSource().try_as<winrt_wux::DependencyObject>();
        while (curNode) {
            if (curNode.try_as<winrt_ctrl::Primitives::ButtonBase>()) {
                return;
            }
            curNode = winrt_med::VisualTreeHelper::GetParent(curNode);
        }

        if (args.GetCurrentPoint(element).Properties().IsLeftButtonPressed()) {
            RECT winRc{};
            if (GetWindowRect(m_hwnd, &winRc) && GetCursorPos(&m_dragStartCursor)) {
                m_dragStartWin.x = winRc.left;
                m_dragStartWin.y = winRc.top;
                m_isDraggingWindow = true;
                element.CapturePointer(args.Pointer());
                args.Handled(true);
            }
        }
    });

    element.PointerMoved([this, element](auto&&, winrt_wux::Input::PointerRoutedEventArgs const& args) {
        if (!m_isDraggingWindow) return;
        const bool mouseDown = (GetKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                               args.GetCurrentPoint(element).Properties().IsLeftButtonPressed();
        if (!mouseDown) {
            m_isDraggingWindow = false;
            element.ReleasePointerCaptures();
            return;
        }
        POINT cur{};
        if (GetCursorPos(&cur)) {
            const int newX = m_dragStartWin.x + (cur.x - m_dragStartCursor.x);
            const int newY = m_dragStartWin.y + (cur.y - m_dragStartCursor.y);
            SetWindowPos(m_hwnd, nullptr, newX, newY, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        args.Handled(true);
    });

    auto stopDrag = [this, element](auto&&, winrt_wux::Input::PointerRoutedEventArgs const& args) {
        if (m_isDraggingWindow) {
            m_isDraggingWindow = false;
            element.ReleasePointerCaptures();
            args.Handled(true);
        }
    };
    element.PointerReleased(stopDrag);
    element.PointerCanceled(stopDrag);
    element.PointerCaptureLost(stopDrag);
}

void AppWindow::LoadLogoSvgIntoUi() {
    auto pngBytes = TrayIconRenderer::LoadLogoPngBytes(m_hInst);
    if (!pngBytes.empty() && m_logoSvgImage) {
        auto loadPngAsync = [](AppWindow* self, std::vector<uint8_t> bytes) -> winrt::fire_and_forget {
            HWND hwnd = self->m_hwnd;
            winrt::apartment_context uiThread;
            try {
                winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
                winrt::Windows::Storage::Streams::DataWriter writer(stream.GetOutputStreamAt(0));
                writer.WriteBytes(bytes);
                co_await writer.StoreAsync();
                writer.DetachStream();
                stream.Seek(0);

                co_await uiThread;
                if (!IsWindow(hwnd) || !self->m_logoSvgImage) co_return;

                winrt_img::BitmapImage bmp;
                bmp.DecodePixelHeight(44);
                self->m_logoSvgImage.Source(bmp);
                co_await bmp.SetSourceAsync(stream);
                if (IsWindow(hwnd) && self->m_logoFallbackBadge) {
                    self->m_logoFallbackBadge.Visibility(winrt_wux::Visibility::Collapsed);
                    self->m_logoSvgImage.Visibility(winrt_wux::Visibility::Visible);
                }
            } catch (...) {
                if (IsWindow(hwnd) && self->m_logoFallbackBadge) {
                    self->m_logoFallbackBadge.Visibility(winrt_wux::Visibility::Visible);
                }
            }
        };
        loadPngAsync(this, std::move(pngBytes));
        return;
    }

    if (m_logoFallbackBadge) {
        m_logoFallbackBadge.Visibility(winrt_wux::Visibility::Visible);
    }
}

void AppWindow::UpdateCustomOptionPills() {
    const bool isPercent = (m_config.trayStyle == L"percent");

    auto activeBg   = winrt_med::SolidColorBrush(RgbColor(109, 40, 217));
    auto activeFg   = winrt_med::SolidColorBrush(RgbColor(255, 255, 255));
    auto inactiveBg = winrt_med::SolidColorBrush(winrt::Windows::UI::Colors::Transparent());
    auto inactiveFg = winrt_med::SolidColorBrush(RgbColor(82, 82, 91));

    if (m_trayModeLogoBtn && m_trayModePercentBtn) {
        m_trayModeLogoBtn.Background(isPercent ? inactiveBg : activeBg);
        m_trayModeLogoBtn.Foreground(isPercent ? inactiveFg : activeFg);
        m_trayModePercentBtn.Background(isPercent ? activeBg : inactiveBg);
        m_trayModePercentBtn.Foreground(isPercent ? activeFg : inactiveFg);
    }

    if (m_thresholdValueText) {
        wchar_t buf[16]{};
        std::swprintf(buf, 16, L"%d%%", m_config.autoPurgeThreshold);
        m_thresholdValueText.Text(winrt::hstring(buf));
    }

    if (m_intervalValueText) {
        if (m_config.autoPurgeIntervalMinutes <= 0) {
            m_intervalValueText.Text(L"Off");
        } else {
            wchar_t buf[16]{};
            std::swprintf(buf, 16, L"%dm", m_config.autoPurgeIntervalMinutes);
            m_intervalValueText.Text(winrt::hstring(buf));
        }
    }
}

void AppWindow::HydrateSettingsUi() {
    m_hydratingUi = true;

    m_tglAutoStart.IsOn(m_config.autoStart);
    m_tglStartMinimized.IsOn(m_config.startMinimized);
    m_tglMinToTray.IsOn(m_config.minimizeToTray);
    m_tglCloseToTray.IsOn(m_config.closeToTray);
    m_tglAlwaysOnTop.IsOn(m_config.alwaysOnTop);

    m_tglWorkingSets.IsOn(m_config.purgeWorkingSets);
    m_tglSystemCache.IsOn(m_config.purgeSystemCache);
    m_tglStandbyList.IsOn(m_config.purgeStandbyList);
    m_tglModifiedList.IsOn(m_config.purgeModifiedList);

    m_tglAutoPurge.IsOn(m_config.autoPurgeEnabled);
    m_tglNotifications.IsOn(m_config.showNotifications);

    UpdateCustomOptionPills();
    m_hydratingUi = false;

    UpdateIdlePurgeStatusText();
}

void AppWindow::SaveAndApplyConfig() {
    m_config.autoStart        = m_tglAutoStart.IsOn();
    m_config.startMinimized   = m_tglStartMinimized.IsOn();
    m_config.minimizeToTray   = m_tglMinToTray.IsOn();
    m_config.closeToTray      = m_tglCloseToTray.IsOn();
    m_config.alwaysOnTop      = m_tglAlwaysOnTop.IsOn();

    m_config.purgeWorkingSets  = m_tglWorkingSets.IsOn();
    m_config.purgeSystemCache  = m_tglSystemCache.IsOn();
    m_config.purgeStandbyList  = m_tglStandbyList.IsOn();
    m_config.purgeModifiedList = m_tglModifiedList.IsOn();

    m_config.autoPurgeEnabled  = m_tglAutoPurge.IsOn();
    m_config.showNotifications = m_tglNotifications.IsOn();

    ConfigManager::SetAutoStartup(m_config.autoStart, m_config.startMinimized);
    ConfigManager::Save(m_config);
    ApplyAlwaysOnTop();
    UpdateTrayIcon(m_lastSnapshot, true);

    if (!m_purgeRunning.load()) {
        UpdateIdlePurgeStatusText();
    }
}

void AppWindow::UpdateIdlePurgeStatusText() {
    if (!m_purgeStatusText) return;
    const bool autoActive = m_config.autoPurgeEnabled || (m_config.autoPurgeIntervalMinutes > 0);
    m_purgeStatusText.Text(autoActive ? L"Auto Purge: ON" : L"Auto Purge: OFF");
    m_purgeStatusText.Foreground(winrt_med::SolidColorBrush(
        autoActive ? RgbColor(109, 40, 217) : RgbColor(113, 113, 122)
    ));
}

void AppWindow::UpdateCustomScrollThumb() {
    if (!m_settingsScrollViewer || !m_customScrollTrack || !m_customScrollThumb || !m_scrollThumbTransform) return;

    const double extent = m_settingsScrollViewer.ExtentHeight();
    const double viewport = m_settingsScrollViewer.ViewportHeight();
    const double trackH = m_customScrollTrack.ActualHeight();
    if (extent <= viewport || viewport <= 0.0 || trackH <= 0.0) {
        m_customScrollThumb.Visibility(winrt_wux::Visibility::Collapsed);
        return;
    }

    m_customScrollThumb.Visibility(winrt_wux::Visibility::Visible);
    const double thumbH = std::max(28.0, trackH * (viewport / extent));
    m_customScrollThumb.Height(thumbH);

    const double maxOffset = extent - viewport;
    const double ratio = (maxOffset > 0.0)
        ? std::clamp(m_settingsScrollViewer.VerticalOffset() / maxOffset, 0.0, 1.0)
        : 0.0;
    m_scrollThumbTransform.Y(ratio * (trackH - thumbH));
}

void AppWindow::SwitchToView(bool showSettings) {
    if (m_showingSettings == showSettings) return;
    m_showingSettings = showSettings;

    if (showSettings) {
        m_mainView.Visibility(winrt_wux::Visibility::Collapsed);
        m_settingsView.Visibility(winrt_wux::Visibility::Visible);
        UpdateCustomScrollThumb();
        if (m_openSettingsSb) {
            m_openSettingsSb.Begin();
        }
    } else {
        m_settingsView.Visibility(winrt_wux::Visibility::Collapsed);
        m_mainView.Visibility(winrt_wux::Visibility::Visible);
        if (m_openMainSb) {
            m_openMainSb.Begin();
        }
    }
}

void AppWindow::ApplyLayeredAlpha(BYTE alpha) {
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) == 0) {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    }
    SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
}

void AppWindow::ClearLayeredStyle() {
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) != 0) {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
}

void AppWindow::StartWindowFade(FadeMode mode) {
    m_fadeMode = mode;
    if (mode == FadeMode::In) {
        const bool wasHidden = !IsWindowVisible(m_hwnd) || IsIconic(m_hwnd);
        if (wasHidden) {
            m_fadeProgress = 0.0;
            ApplyLayeredAlpha(0);
            ShowWindow(m_hwnd, SW_RESTORE);
            SetForegroundWindow(m_hwnd);
            if (!m_showingSettings && m_startupSloganSb) {
                m_startupSloganSb.Begin();
            }
        }
        SetTimer(m_hwnd, IDT_WINDOW_FADE, kFadeTimerMs, nullptr);
    } else if (mode == FadeMode::OutHide || mode == FadeMode::OutMinimize || mode == FadeMode::OutClose) {
        if (!IsWindowVisible(m_hwnd)) {
            m_fadeMode = FadeMode::None;
            if (mode == FadeMode::OutClose) {
                m_closingForReal = true;
                DestroyWindow(m_hwnd);
            }
            return;
        }
        if (m_fadeProgress <= 0.0 || m_fadeProgress > 1.0) {
            m_fadeProgress = 1.0;
        }
        ApplyLayeredAlpha(static_cast<BYTE>(std::lround(m_fadeProgress * 255.0)));
        SetTimer(m_hwnd, IDT_WINDOW_FADE, kFadeTimerMs, nullptr);
    }
}

void AppWindow::ShowMainWindow() {
    StartWindowFade(FadeMode::In);
}

void AppWindow::HideToTray() {
    StartWindowFade(FadeMode::OutHide);
}

void AppWindow::MinimizeWindow() {
    StartWindowFade(FadeMode::OutMinimize);
}

void AppWindow::RefreshMemorySnapshot(bool checkAutoPurge) {
    const MemorySnapshot snap = MemoryEngine::Query();
    m_lastSnapshot = snap;

    const std::wstring usedStr  = MemoryEngine::FormatBytes(snap.usedBytes);
    const std::wstring totalStr = MemoryEngine::FormatBytes(snap.totalBytes);
    const std::wstring availStr = MemoryEngine::FormatBytes(snap.availBytes);
    const std::wstring cacheStr = MemoryEngine::FormatBytes(snap.cachedBytes);

    m_usageRatioText.Text(winrt::hstring(usedStr + L" / " + totalStr));
    m_inUseText.Text(winrt::hstring(usedStr));
    m_availableText.Text(winrt::hstring(availStr));
    m_cachedText.Text(winrt::hstring(cacheStr));

    m_targetPercent = static_cast<double>(snap.usagePercent);
    if (std::fabs(m_displayedPercent - m_targetPercent) > 0.25) {
        SetTimer(m_hwnd, IDT_ARC_ANIM, kArcAnimMs, nullptr);
    } else {
        UpdateGaugeVisuals(m_targetPercent);
    }

    UpdateTrayIcon(snap, false);

    if (checkAutoPurge && !m_purgeRunning.load()) {
        const uint64_t nowMs = GetTickCount64();
        const uint64_t elapsedMs = nowMs - m_lastPurgeTickMs;

        const bool hitThreshold = m_config.autoPurgeEnabled &&
                                  snap.usagePercent >= static_cast<uint32_t>(m_config.autoPurgeThreshold) &&
                                  elapsedMs >= 60'000ULL;

        const bool hitPeriodic = (m_config.autoPurgeIntervalMinutes > 0) &&
                                 elapsedMs >= static_cast<uint64_t>(m_config.autoPurgeIntervalMinutes) * 60'000ULL;

        if (hitThreshold || hitPeriodic) {
            TriggerPurgeAsync(true);
        }
    }
}

void AppWindow::UpdateGaugeVisuals(double percent) {
    const double clamped = std::clamp(percent, 0.0, 100.0);

    if (m_usagePercentText) {
        wchar_t pctBuf[16]{};
        std::swprintf(pctBuf, 16, L"%u%%", static_cast<unsigned>(std::lround(clamped)));
        m_usagePercentText.Text(winrt::hstring(pctBuf));
    }

    if (!m_usageArcPath) return;

    if (clamped < 0.5) {
        m_usageArcPath.Data(nullptr);
        return;
    }

    constexpr double cx = 74.0;
    constexpr double cy = 74.0;
    constexpr double r  = 70.0;
    constexpr double pi = 3.14159265358979323846;

    const double sweepDeg = std::min(359.95, (clamped / 100.0) * 360.0);
    const double endRad = (sweepDeg - 90.0) * (pi / 180.0);

    winrt::Windows::Foundation::Point startPt{static_cast<float>(cx), static_cast<float>(cy - r)};
    winrt::Windows::Foundation::Point endPt{
        static_cast<float>(cx + r * std::cos(endRad)),
        static_cast<float>(cy + r * std::sin(endRad))
    };

    winrt_med::ArcSegment arc;
    arc.Point(endPt);
    arc.Size(winrt::Windows::Foundation::Size{static_cast<float>(r), static_cast<float>(r)});
    arc.IsLargeArc(sweepDeg > 180.0);
    arc.SweepDirection(winrt_med::SweepDirection::Clockwise);

    winrt_med::PathFigure figure;
    figure.StartPoint(startPt);
    figure.IsClosed(false);
    figure.Segments().Append(arc);

    winrt_med::PathGeometry geometry;
    geometry.Figures().Append(figure);
    m_usageArcPath.Data(geometry);
}

void AppWindow::TriggerPurgeAsync(bool fromAuto) {
    bool expected = false;
    if (!m_purgeRunning.compare_exchange_strong(expected, true)) {
        return;
    }

    KillTimer(m_hwnd, IDT_STATUS_RESET);
    if (m_purgePulseSb) {
        m_purgePulseSb.Begin();
    }
    if (m_purgeBtn) m_purgeBtn.IsEnabled(false);
    if (m_purgeBtnText) m_purgeBtnText.Text(L"Purging...");
    if (m_purgeStatusText) {
        m_purgeStatusText.Text(fromAuto ? L"Auto-purging..." : L"Purging...");
        m_purgeStatusText.Foreground(winrt_med::SolidColorBrush(RgbColor(113, 113, 122)));
    }

    PurgeOptions opts{};
    opts.workingSets  = m_config.purgeWorkingSets;
    opts.systemCache  = m_config.purgeSystemCache;
    opts.standbyList  = m_config.purgeStandbyList;
    opts.modifiedList = m_config.purgeModifiedList;

    HWND targetHwnd = m_hwnd;
    std::thread([targetHwnd, opts]() {
        auto* result = new PurgeResult(MemoryEngine::Purge(opts));
        if (!PostMessageW(targetHwnd, WM_APP_PURGE_DONE, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void AppWindow::InitTrayIcon() {
    m_lastSnapshot = MemoryEngine::Query();

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;

    if (m_config.trayStyle == L"percent") {
        m_currentTrayIcon = m_trayRenderer.CreateUsagePercentIcon(m_lastSnapshot.usagePercent, m_dpi);
    } else {
        m_currentTrayIcon = m_trayRenderer.CreateLogoIcon(m_hInst, m_dpi);
    }
    nid.hIcon = m_currentTrayIcon ? m_currentTrayIcon : m_appIconSmall;
    wcscpy_s(nid.szTip, L"VRam V1.0");

    m_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    m_lastTrayPercent = m_lastSnapshot.usagePercent;
    m_lastTrayStyle = m_config.trayStyle;
}

void AppWindow::UpdateTrayIcon(const MemorySnapshot& snap, bool forceIconRecreate) {
    if (!m_trayAdded) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP;

    const std::wstring usedStr  = MemoryEngine::FormatBytes(snap.usedBytes);
    const std::wstring totalStr = MemoryEngine::FormatBytes(snap.totalBytes);
    std::swprintf(nid.szTip, ARRAYSIZE(nid.szTip),
                  L"VRam V1.0 - RAM: %u%% (%s / %s)",
                  snap.usagePercent, usedStr.c_str(), totalStr.c_str());

    const bool styleChanged = (m_lastTrayStyle != m_config.trayStyle);
    const bool percentChanged = (m_config.trayStyle == L"percent" && m_lastTrayPercent != snap.usagePercent);

    if (forceIconRecreate || styleChanged || percentChanged) {
        HICON nextIcon = (m_config.trayStyle == L"percent")
            ? m_trayRenderer.CreateUsagePercentIcon(snap.usagePercent, m_dpi)
            : m_trayRenderer.CreateLogoIcon(m_hInst, m_dpi);

        if (nextIcon) {
            if (m_currentTrayIcon) {
                DestroyIcon(m_currentTrayIcon);
            }
            m_currentTrayIcon = nextIcon;
            nid.uFlags |= NIF_ICON;
            nid.hIcon = m_currentTrayIcon;
        }
        m_lastTrayPercent = snap.usagePercent;
        m_lastTrayStyle = m_config.trayStyle;
    }

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AppWindow::RemoveTrayIcon() {
    if (!m_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_trayAdded = false;
}

void AppWindow::ShowTrayBalloon(const wchar_t* title, const wchar_t* body) {
    if (!m_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, body, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void AppWindow::ShowTrayContextMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    const bool visible = IsWindowVisible(m_hwnd) != FALSE;
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, visible ? L"Hide to Tray" : L"Show VRam");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_PURGE, L"Purge Memory Now");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SETTINGS, L"Settings");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit VRam");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

}
