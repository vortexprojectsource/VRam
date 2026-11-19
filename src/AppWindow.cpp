#include "AppWindow.hpp"
#include "../resources/resource.h"

#include <dwmapi.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
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

constexpr wchar_t kWindowClassName[] = L"VRam_V2_HostWindow";
constexpr int kClientDipWidth  = 368;
constexpr int kClientDipHeight = 524;

constexpr UINT_PTR IDT_POLL_MEMORY    = 1001;
constexpr UINT_PTR IDT_ARC_ANIM       = 1002;
constexpr UINT_PTR IDT_WINDOW_FADE    = 1003;
constexpr UINT_PTR IDT_STATUS_RESET   = 1004;
constexpr UINT_PTR IDT_TOAST_COMPLETE = 1005;
constexpr UINT     kPollIntervalMs    = 2000;
constexpr UINT     kArcAnimMs         = 16;
constexpr UINT     kFadeTimerMs       = 12;
constexpr UINT     kStatusResetMs     = 4000;
constexpr uint64_t kToastMinRunMs     = 550;

constexpr UINT kTrayIconId = 1;
constexpr UINT IDM_TRAY_SHOW     = 40001;
constexpr UINT IDM_TRAY_PURGE    = 40002;
constexpr UINT IDM_TRAY_GAMES    = 40003;
constexpr UINT IDM_TRAY_SETTINGS = 40004;
constexpr UINT IDM_TRAY_EXIT     = 40005;

constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmRoundCorner            = 2;
constexpr DWORD kDwmBorderColor            = 34;

constexpr std::array<int, 5> kThresholds = {70, 75, 80, 85, 90};
constexpr std::array<int, 5> kIntervals  = {0, 5, 15, 30, 60};

winrt::Windows::UI::Color RgbColor(uint8_t r, uint8_t g, uint8_t b) {
    return winrt::Windows::UI::ColorHelper::FromArgb(255, r, g, b);
}

std::wstring NormalizeExeFilename(const std::wstring& pathOrName) {
    if (pathOrName.empty()) return {};
    const wchar_t* leaf = PathFindFileNameW(pathOrName.c_str());
    return (leaf && *leaf) ? std::wstring(leaf) : pathOrName;
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
    if (m_dpi == 0) {
        m_dpi = 96;
    }
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
        L"VRam V2.0",
        style,
        posX, posY, winW, winH,
        nullptr, nullptr, m_hInst, this
    );
    if (!m_hwnd || m_xamlInitFailed) {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        return false;
    }

    m_toastWindow.Create(m_hInst, m_hwnd, WM_APP_SHOW_WINDOW);

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
        case IDM_TRAY_GAMES:
            ShowMainWindow();
            SwitchToView(ActiveView::Games);
            return 0;
        case IDM_TRAY_SETTINGS:
            ShowMainWindow();
            SwitchToView(ActiveView::Settings);
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
    if (m_dpi == 0) {
        m_dpi = 96;
    }

    try {
        m_xamlManager = winrt_wux::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        m_xamlSource = winrt_wux::Hosting::DesktopWindowXamlSource();

        auto native = m_xamlSource.try_as<IDesktopWindowXamlSourceNative>();
        if (!native) {
            winrt::throw_hresult(E_NOINTERFACE);
        }
        winrt::check_hresult(native->AttachToWindow(m_hwnd));
        winrt::check_hresult(native->get_WindowHandle(&m_hwndXamlIsland));

        const std::wstring xamlXml = LoadUtf8ResourceAsWide(m_hInst, IDR_XAML_LAYOUT);
        if (xamlXml.empty()) {
            winrt::throw_hresult(ERROR_RESOURCE_DATA_NOT_FOUND);
        }
        auto parsed = winrt_wux::Markup::XamlReader::Load(winrt::hstring(xamlXml));
        m_root = parsed.try_as<winrt_wux::FrameworkElement>();
        if (!m_root) {
            winrt::throw_hresult(E_NOINTERFACE);
        }
        m_xamlSource.Content(m_root);

        ResizeXamlIsland();
        BindXamlControls();
        LoadLogoSvgIntoUi();
        HydrateSettingsUi();

        InitTrayIcon();
        RefreshMemorySnapshot(false);
        RefreshGameProtectionUi(m_config.maximizeGameRam, true);
        m_displayedPercent = m_targetPercent;
        UpdateGaugeVisuals(m_displayedPercent);

        SetTimer(m_hwnd, IDT_POLL_MEMORY, kPollIntervalMs, nullptr);
    } catch (const winrt::hresult_error& err) {
        m_xamlInitFailed = true;
        wchar_t buf[512]{};
        _snwprintf_s(buf, _TRUNCATE,
                     L"VRam V2.0 UI engine (XAML Islands) failed to start on this device.\n\n"
                     L"Detail: %s (HRESULT 0x%08X)\n\n"
                     L"Windows 10 build 18362+ (1903) or Windows 11 with XAML framework is required.",
                     err.message().c_str(),
                     static_cast<unsigned int>(err.code()));
        MessageBoxW(m_hwnd, buf, L"VRam V2.0 - Startup Error", MB_OK | MB_ICONERROR);
    } catch (const std::exception& ex) {
        m_xamlInitFailed = true;
        wchar_t wbuf[256]{};
        MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, wbuf, 256);
        MessageBoxW(m_hwnd, wbuf, L"VRam V2.0 - Startup Error", MB_OK | MB_ICONERROR);
    } catch (...) {
        m_xamlInitFailed = true;
        MessageBoxW(m_hwnd,
                    L"VRam V2.0 encountered an unexpected initialization error.",
                    L"VRam V2.0 - Startup Error",
                    MB_OK | MB_ICONERROR);
    }
}

void AppWindow::OnDestroy() {
    KillTimer(m_hwnd, IDT_POLL_MEMORY);
    KillTimer(m_hwnd, IDT_ARC_ANIM);
    KillTimer(m_hwnd, IDT_WINDOW_FADE);
    KillTimer(m_hwnd, IDT_STATUS_RESET);
    KillTimer(m_hwnd, IDT_TOAST_COMPLETE);
    m_pendingToastResult.reset();
    m_toastWindow.Destroy();
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
    m_dpi = (newDpi > 0) ? newDpi : 96;
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
    } else if (timerId == IDT_TOAST_COMPLETE) {
        KillTimer(m_hwnd, IDT_TOAST_COMPLETE);
        if (m_pendingToastResult) {
            if (m_config.showNotifications) {
                m_toastWindow.ShowCompleted(*m_pendingToastResult);
            }
            m_pendingToastResult.reset();
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
            if (!m_isElevated && res->privilegeLimited) {
                _snwprintf_s(msg, _TRUNCATE, L"Freed %s (%u%% -> %u%%) | Elevate for standby",
                             freedStr.c_str(), res->beforePercent, res->afterPercent);
            } else if (res->protectedProcesses > 0 && !res->activeGameName.empty()) {
                _snwprintf_s(msg, _TRUNCATE, L"Freed %s | %s shielded",
                             freedStr.c_str(), res->activeGameName.c_str());
            } else {
                _snwprintf_s(msg, _TRUNCATE, L"Freed %s (%u%% -> %u%%)",
                             freedStr.c_str(), res->beforePercent, res->afterPercent);
            }
        } else if (!m_isElevated && res->privilegeLimited) {
            _snwprintf_s(msg, _TRUNCATE, L"Trimmed %u processes | Elevate for standby",
                         res->trimmedProcesses);
        } else if (res->protectedProcesses > 0 && !res->activeGameName.empty()) {
            _snwprintf_s(msg, _TRUNCATE, L"Standby cleaned | %s kept maximized",
                         res->activeGameName.c_str());
        } else {
            _snwprintf_s(msg, _TRUNCATE, L"Already compact (%u%%)", res->afterPercent);
        }

        m_purgeStatusText.Text(winrt::hstring(msg));
        m_purgeStatusText.Foreground(winrt_med::SolidColorBrush(
            res->freedBytes > 0 ? RgbColor(109, 40, 217) : RgbColor(113, 113, 122)
        ));

        if (m_config.showNotifications) {
            const uint64_t elapsed = GetTickCount64() - m_toastPurgeStartTick;
            if (elapsed >= kToastMinRunMs) {
                KillTimer(m_hwnd, IDT_TOAST_COMPLETE);
                m_pendingToastResult.reset();
                m_toastWindow.ShowCompleted(*res);
            } else {
                m_pendingToastResult = std::make_unique<PurgeResult>(*res);
                SetTimer(m_hwnd, IDT_TOAST_COMPLETE,
                         static_cast<UINT>(kToastMinRunMs - elapsed), nullptr);
            }
        }
        SetTimer(m_hwnd, IDT_STATUS_RESET, kStatusResetMs, nullptr);
    }

    RefreshMemorySnapshot(false);
    RefreshGameProtectionUi(false, m_activeView == ActiveView::Games);
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
    m_openGamesSb     = findSb(L"OpenGamesStoryboard");

    m_titleBarDragArea       = find(L"TitleBarDragArea").try_as<winrt_ctrl::Grid>();
    m_settingsHeaderDragArea = find(L"SettingsHeaderDragArea").try_as<winrt_ctrl::Grid>();
    m_gameHeaderDragArea     = find(L"GameHeaderDragArea").try_as<winrt_ctrl::Grid>();
    m_logoFallbackBadge      = find(L"LogoFallbackBadge").try_as<winrt_ctrl::Viewbox>();
    m_logoSvgImage           = find(L"LogoSvgImage").try_as<winrt_ctrl::Image>();
    m_winMinimizeBtn         = find(L"WinMinimizeButton").try_as<winrt_ctrl::Button>();
    m_winCloseBtn            = find(L"WinCloseButton").try_as<winrt_ctrl::Button>();

    m_mainView               = find(L"MainView").try_as<winrt_ctrl::Grid>();
    m_settingsView           = find(L"SettingsView").try_as<winrt_ctrl::Grid>();
    m_gameView               = find(L"GameView").try_as<winrt_ctrl::Grid>();
    m_usageArcPath           = find(L"UsageArcPath").try_as<winrt_shp::Path>();
    m_usagePercentText       = find(L"UsagePercentText").try_as<winrt_ctrl::TextBlock>();
    m_usageRatioText         = find(L"UsageRatioText").try_as<winrt_ctrl::TextBlock>();
    m_inUseText              = find(L"InUseText").try_as<winrt_ctrl::TextBlock>();
    m_availableText          = find(L"AvailableText").try_as<winrt_ctrl::TextBlock>();
    m_cachedText             = find(L"CachedText").try_as<winrt_ctrl::TextBlock>();
    m_gameShieldBtn          = find(L"GameShieldButton").try_as<winrt_ctrl::Button>();
    m_gameShieldTitleText    = find(L"GameShieldTitleText").try_as<winrt_ctrl::TextBlock>();
    m_gameShieldSubText      = find(L"GameShieldSubText").try_as<winrt_ctrl::TextBlock>();
    m_gameShieldBadgeBorder  = find(L"GameShieldBadgeBorder").try_as<winrt_ctrl::Border>();
    m_gameShieldBadgeText    = find(L"GameShieldBadgeText").try_as<winrt_ctrl::TextBlock>();
    m_purgeBtn               = find(L"PurgeButton").try_as<winrt_ctrl::Button>();
    m_purgeBtnText           = find(L"PurgeButtonText").try_as<winrt_ctrl::TextBlock>();
    m_purgeStatusText        = find(L"PurgeStatusText").try_as<winrt_ctrl::TextBlock>();
    m_settingsBtn            = find(L"SettingsButton").try_as<winrt_ctrl::Button>();
    m_minTrayBtn             = find(L"MinimizeTrayButton").try_as<winrt_ctrl::Button>();

    m_gameBackBtn            = find(L"GameBackButton").try_as<winrt_ctrl::Button>();
    m_boostGamesNowBtn       = find(L"BoostGamesNowBtn").try_as<winrt_ctrl::Button>();
    m_gameScrollViewer       = find(L"GameScrollViewer").try_as<winrt_ctrl::ScrollViewer>();
    m_gameScrollTrack        = find(L"GameScrollTrack").try_as<winrt_ctrl::Grid>();
    m_gameScrollThumb        = find(L"GameScrollThumb").try_as<winrt_ctrl::Border>();
    m_gameScrollThumbTransform = find(L"GameScrollThumbTransform").try_as<winrt_med::TranslateTransform>();
    m_tglMaximizeGame        = find(L"ToggleMaximizeGame").try_as<winrt_ctrl::ToggleSwitch>();
    m_browseGameBtn          = find(L"BrowseGameBtn").try_as<winrt_ctrl::Button>();
    m_refreshProcsBtn        = find(L"RefreshProcsBtn").try_as<winrt_ctrl::Button>();
    m_gameActionStatusText   = find(L"GameActionStatusText").try_as<winrt_ctrl::TextBlock>();
    m_noGamesEmptyCard       = find(L"NoGamesEmptyCard").try_as<winrt_ctrl::Border>();
    m_protectedGamesList     = find(L"ProtectedGamesList").try_as<winrt_ctrl::StackPanel>();
    m_runningProcessesList   = find(L"RunningProcessesList").try_as<winrt_ctrl::StackPanel>();

    m_backBtn                = find(L"BackButton").try_as<winrt_ctrl::Button>();
    m_settingsScrollViewer   = find(L"SettingsScrollViewer").try_as<winrt_ctrl::ScrollViewer>();
    m_customScrollTrack      = find(L"CustomScrollTrack").try_as<winrt_ctrl::Grid>();
    m_customScrollThumb      = find(L"CustomScrollThumb").try_as<winrt_ctrl::Border>();
    m_scrollThumbTransform   = find(L"ScrollThumbTransform").try_as<winrt_med::TranslateTransform>();
    m_tglAutoStart           = find(L"ToggleAutoStart").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglStartMinimized      = find(L"ToggleStartMinimized").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglMinToTray           = find(L"ToggleMinimizeToTray").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglCloseToTray         = find(L"ToggleCloseToTray").try_as<winrt_ctrl::ToggleSwitch>();
    m_trayModeLogoBtn        = find(L"TrayModeLogoBtn").try_as<winrt_ctrl::Button>();
    m_trayModePercentBtn     = find(L"TrayModePercentBtn").try_as<winrt_ctrl::Button>();
    m_tglAlwaysOnTop         = find(L"ToggleAlwaysOnTop").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglWorkingSets         = find(L"ToggleWorkingSets").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglSystemCache         = find(L"ToggleSystemCache").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglStandbyList         = find(L"ToggleStandbyList").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglModifiedList        = find(L"ToggleModifiedList").try_as<winrt_ctrl::ToggleSwitch>();
    m_tglAutoPurge           = find(L"ToggleAutoPurge").try_as<winrt_ctrl::ToggleSwitch>();
    m_thresholdPrevBtn       = find(L"ThresholdPrevBtn").try_as<winrt_ctrl::Button>();
    m_thresholdNextBtn       = find(L"ThresholdNextBtn").try_as<winrt_ctrl::Button>();
    m_thresholdValueText     = find(L"ThresholdValueText").try_as<winrt_ctrl::TextBlock>();
    m_intervalPrevBtn        = find(L"IntervalPrevBtn").try_as<winrt_ctrl::Button>();
    m_intervalNextBtn        = find(L"IntervalNextBtn").try_as<winrt_ctrl::Button>();
    m_intervalValueText      = find(L"IntervalValueText").try_as<winrt_ctrl::TextBlock>();
    m_tglNotifications       = find(L"ToggleNotifications").try_as<winrt_ctrl::ToggleSwitch>();
    m_settingsElevateRow     = find(L"SettingsElevateRow").try_as<winrt_ctrl::Grid>();
    m_settingsElevateBtn     = find(L"SettingsElevateButton").try_as<winrt_ctrl::Button>();
    m_supportDevBtn          = find(L"SupportDevButton").try_as<winrt_ctrl::Button>();

    AttachDragHandlers(m_titleBarDragArea);
    AttachDragHandlers(m_settingsHeaderDragArea);
    AttachDragHandlers(m_gameHeaderDragArea);
    AttachDragHandlers(m_mainView);

    if (m_winMinimizeBtn) {
        m_winMinimizeBtn.Click([this](auto&&, auto&&) {
            if (m_config.minimizeToTray) {
                HideToTray();
            } else {
                MinimizeWindow();
            }
        });
    }

    if (m_winCloseBtn) {
        m_winCloseBtn.Click([this](auto&&, auto&&) {
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        });
    }

    if (m_settingsElevateRow) {
        m_settingsElevateRow.Visibility(m_isElevated ? winrt_wux::Visibility::Collapsed : winrt_wux::Visibility::Visible);
    }

    if (m_settingsElevateBtn) {
        m_settingsElevateBtn.Click([this](winrt::Windows::Foundation::IInspectable const&, winrt_wux::RoutedEventArgs const&) {
            if (m_isElevated) return;
            if (MemoryEngine::RestartElevated(false)) {
                m_closingForReal = true;
                DestroyWindow(m_hwnd);
            }
        });
    }

    if (m_supportDevBtn) {
        m_supportDevBtn.Click([](auto&&, auto&&) {
            ShellExecuteW(nullptr, L"open", L"https://sociabuzz.com/idnvinnmalay", nullptr, nullptr, SW_SHOWNORMAL);
        });
    }

    if (m_settingsScrollViewer) {
        m_settingsScrollViewer.ViewChanged([this](auto&&, auto&&) { UpdateCustomScrollThumb(); });
        m_settingsScrollViewer.SizeChanged([this](auto&&, auto&&) { UpdateCustomScrollThumb(); });
    }
    if (m_customScrollTrack) {
        m_customScrollTrack.SizeChanged([this](auto&&, auto&&) { UpdateCustomScrollThumb(); });
    }

    if (m_gameScrollViewer) {
        m_gameScrollViewer.ViewChanged([this](auto&&, auto&&) { UpdateGameScrollThumb(); });
        m_gameScrollViewer.SizeChanged([this](auto&&, auto&&) { UpdateGameScrollThumb(); });
    }
    if (m_gameScrollTrack) {
        m_gameScrollTrack.SizeChanged([this](auto&&, auto&&) { UpdateGameScrollThumb(); });
    }

    if (m_purgeBtn) {
        m_purgeBtn.Click([this](auto&&, auto&&) {
            TriggerPurgeAsync(false);
        });
    }

    if (m_gameShieldBtn) {
        m_gameShieldBtn.Click([this](auto&&, auto&&) {
            SwitchToView(ActiveView::Games);
        });
    }

    if (m_gameBackBtn) {
        m_gameBackBtn.Click([this](auto&&, auto&&) {
            SwitchToView(ActiveView::Main);
        });
    }

    if (m_browseGameBtn) {
        m_browseGameBtn.Click([this](auto&&, auto&&) {
            BrowseForGameExe();
        });
    }

    if (m_refreshProcsBtn) {
        m_refreshProcsBtn.Click([this](auto&&, auto&&) {
            RefreshGameProtectionUi(false, true);
        });
    }

    if (m_boostGamesNowBtn) {
        m_boostGamesNowBtn.Click([this](auto&&, auto&&) {
            MemoryEngine::EnableAvailablePrivileges();
            RefreshGameProtectionUi(true, true);
            if (m_gameActionStatusText) {
                if (m_config.protectedGames.empty()) {
                    m_gameActionStatusText.Text(L"Add at least one game .exe first to boost.");
                } else if (m_lastGameStatus.anyRunning) {
                    wchar_t buf[160]{};
                    _snwprintf_s(buf, _TRUNCATE, L"Boosted %s (%s kept resident in RAM)",
                                 m_lastGameStatus.primaryGameName.c_str(),
                                 MemoryEngine::FormatBytes(m_lastGameStatus.combinedWorkingSetBytes).c_str());
                    m_gameActionStatusText.Text(winrt::hstring(buf));
                } else {
                    m_gameActionStatusText.Text(L"Armed: game will auto-boost on launch.");
                }
            }
        });
    }

    if (m_settingsBtn) {
        m_settingsBtn.Click([this](auto&&, auto&&) {
            SwitchToView(ActiveView::Settings);
        });
        m_settingsBtn.PointerEntered([this](auto&&, auto&&) {
            if (m_gearSpinInSb) m_gearSpinInSb.Begin();
        });
        m_settingsBtn.PointerExited([this](auto&&, auto&&) {
            if (m_gearSpinOutSb) m_gearSpinOutSb.Begin();
        });
    }

    if (m_backBtn) {
        m_backBtn.Click([this](auto&&, auto&&) {
            SwitchToView(ActiveView::Main);
        });
    }

    if (m_minTrayBtn) {
        m_minTrayBtn.Click([this](auto&&, auto&&) {
            HideToTray();
        });
    }

    auto onToggleChanged = [this](auto&&, auto&&) {
        if (!m_hydratingUi) SaveAndApplyConfig();
    };

    if (m_tglAutoStart)      m_tglAutoStart.Toggled(onToggleChanged);
    if (m_tglStartMinimized) m_tglStartMinimized.Toggled(onToggleChanged);
    if (m_tglMinToTray)      m_tglMinToTray.Toggled(onToggleChanged);
    if (m_tglCloseToTray)    m_tglCloseToTray.Toggled(onToggleChanged);
    if (m_tglAlwaysOnTop)    m_tglAlwaysOnTop.Toggled(onToggleChanged);
    if (m_tglWorkingSets)    m_tglWorkingSets.Toggled(onToggleChanged);
    if (m_tglSystemCache)    m_tglSystemCache.Toggled(onToggleChanged);
    if (m_tglStandbyList)    m_tglStandbyList.Toggled(onToggleChanged);
    if (m_tglModifiedList)   m_tglModifiedList.Toggled(onToggleChanged);
    if (m_tglAutoPurge)      m_tglAutoPurge.Toggled(onToggleChanged);
    if (m_tglNotifications)  m_tglNotifications.Toggled(onToggleChanged);
    if (m_tglMaximizeGame)   m_tglMaximizeGame.Toggled(onToggleChanged);

    if (m_trayModeLogoBtn) {
        m_trayModeLogoBtn.Click([this](auto&&, auto&&) {
            m_config.trayStyle = L"logo";
            UpdateCustomOptionPills();
            SaveAndApplyConfig();
        });
    }
    if (m_trayModePercentBtn) {
        m_trayModePercentBtn.Click([this](auto&&, auto&&) {
            m_config.trayStyle = L"percent";
            UpdateCustomOptionPills();
            SaveAndApplyConfig();
        });
    }

    if (m_thresholdPrevBtn) {
        m_thresholdPrevBtn.Click([this](auto&&, auto&&) {
            m_config.autoPurgeThreshold = StepOption(kThresholds, m_config.autoPurgeThreshold, -1);
            UpdateCustomOptionPills();
            SaveAndApplyConfig();
        });
    }
    if (m_thresholdNextBtn) {
        m_thresholdNextBtn.Click([this](auto&&, auto&&) {
            m_config.autoPurgeThreshold = StepOption(kThresholds, m_config.autoPurgeThreshold, +1);
            UpdateCustomOptionPills();
            SaveAndApplyConfig();
        });
    }

    if (m_intervalPrevBtn) {
        m_intervalPrevBtn.Click([this](auto&&, auto&&) {
            m_config.autoPurgeIntervalMinutes = StepOption(kIntervals, m_config.autoPurgeIntervalMinutes, -1);
            UpdateCustomOptionPills();
            SaveAndApplyConfig();
        });
    }
    if (m_intervalNextBtn) {
        m_intervalNextBtn.Click([this](auto&&, auto&&) {
            m_config.autoPurgeIntervalMinutes = StepOption(kIntervals, m_config.autoPurgeIntervalMinutes, +1);
            UpdateCustomOptionPills();
            SaveAndApplyConfig();
        });
    }
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
        _snwprintf_s(buf, _TRUNCATE, L"%d%%", m_config.autoPurgeThreshold);
        m_thresholdValueText.Text(winrt::hstring(buf));
    }

    if (m_intervalValueText) {
        if (m_config.autoPurgeIntervalMinutes <= 0) {
            m_intervalValueText.Text(L"Off");
        } else {
            wchar_t buf[16]{};
            _snwprintf_s(buf, _TRUNCATE, L"%dm", m_config.autoPurgeIntervalMinutes);
            m_intervalValueText.Text(winrt::hstring(buf));
        }
    }
}

void AppWindow::HydrateSettingsUi() {
    m_hydratingUi = true;

    if (m_tglAutoStart)      m_tglAutoStart.IsOn(m_config.autoStart);
    if (m_tglStartMinimized) m_tglStartMinimized.IsOn(m_config.startMinimized);
    if (m_tglMinToTray)      m_tglMinToTray.IsOn(m_config.minimizeToTray);
    if (m_tglCloseToTray)    m_tglCloseToTray.IsOn(m_config.closeToTray);
    if (m_tglAlwaysOnTop)    m_tglAlwaysOnTop.IsOn(m_config.alwaysOnTop);

    if (m_tglWorkingSets)    m_tglWorkingSets.IsOn(m_config.purgeWorkingSets);
    if (m_tglSystemCache)    m_tglSystemCache.IsOn(m_config.purgeSystemCache);
    if (m_tglStandbyList)    m_tglStandbyList.IsOn(m_config.purgeStandbyList);
    if (m_tglModifiedList)   m_tglModifiedList.IsOn(m_config.purgeModifiedList);

    if (m_tglAutoPurge)      m_tglAutoPurge.IsOn(m_config.autoPurgeEnabled);
    if (m_tglNotifications)  m_tglNotifications.IsOn(m_config.showNotifications);
    if (m_tglMaximizeGame)   m_tglMaximizeGame.IsOn(m_config.maximizeGameRam);

    UpdateCustomOptionPills();
    m_hydratingUi = false;

    UpdateIdlePurgeStatusText();
}

void AppWindow::SaveAndApplyConfig() {
    if (m_tglAutoStart)      m_config.autoStart        = m_tglAutoStart.IsOn();
    if (m_tglStartMinimized) m_config.startMinimized   = m_tglStartMinimized.IsOn();
    if (m_tglMinToTray)      m_config.minimizeToTray   = m_tglMinToTray.IsOn();
    if (m_tglCloseToTray)    m_config.closeToTray      = m_tglCloseToTray.IsOn();
    if (m_tglAlwaysOnTop)    m_config.alwaysOnTop      = m_tglAlwaysOnTop.IsOn();

    if (m_tglWorkingSets)    m_config.purgeWorkingSets  = m_tglWorkingSets.IsOn();
    if (m_tglSystemCache)    m_config.purgeSystemCache  = m_tglSystemCache.IsOn();
    if (m_tglStandbyList)    m_config.purgeStandbyList  = m_tglStandbyList.IsOn();
    if (m_tglModifiedList)   m_config.purgeModifiedList = m_tglModifiedList.IsOn();

    if (m_tglAutoPurge)      m_config.autoPurgeEnabled  = m_tglAutoPurge.IsOn();
    if (m_tglNotifications)  m_config.showNotifications = m_tglNotifications.IsOn();
    if (m_tglMaximizeGame)   m_config.maximizeGameRam   = m_tglMaximizeGame.IsOn();

    ConfigManager::SetAutoStartup(m_config.autoStart, m_config.startMinimized);
    ConfigManager::Save(m_config);
    ApplyAlwaysOnTop();
    UpdateTrayIcon(m_lastSnapshot, true);
    RefreshGameProtectionUi(m_config.maximizeGameRam, false);

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

void AppWindow::UpdateScrollThumbFor(
    const winrt_ctrl::ScrollViewer& sv,
    const winrt_ctrl::Grid& track,
    const winrt_ctrl::Border& thumb,
    const winrt_med::TranslateTransform& tt) {
    if (!sv || !track || !thumb || !tt) return;

    const double extent = sv.ExtentHeight();
    const double viewport = sv.ViewportHeight();
    const double trackH = track.ActualHeight();
    if (extent <= viewport || viewport <= 0.0 || trackH <= 0.0) {
        thumb.Visibility(winrt_wux::Visibility::Collapsed);
        return;
    }

    thumb.Visibility(winrt_wux::Visibility::Visible);
    const double thumbH = std::max(28.0, trackH * (viewport / extent));
    thumb.Height(thumbH);

    const double maxOffset = extent - viewport;
    const double ratio = (maxOffset > 0.0)
        ? std::clamp(sv.VerticalOffset() / maxOffset, 0.0, 1.0)
        : 0.0;
    tt.Y(ratio * (trackH - thumbH));
}

void AppWindow::UpdateCustomScrollThumb() {
    UpdateScrollThumbFor(m_settingsScrollViewer, m_customScrollTrack, m_customScrollThumb, m_scrollThumbTransform);
}

void AppWindow::UpdateGameScrollThumb() {
    UpdateScrollThumbFor(m_gameScrollViewer, m_gameScrollTrack, m_gameScrollThumb, m_gameScrollThumbTransform);
}

void AppWindow::SwitchToView(ActiveView view) {
    if (m_activeView == view) return;
    m_activeView = view;

    if (m_mainView) {
        m_mainView.Visibility(view == ActiveView::Main ? winrt_wux::Visibility::Visible : winrt_wux::Visibility::Collapsed);
    }
    if (m_settingsView) {
        m_settingsView.Visibility(view == ActiveView::Settings ? winrt_wux::Visibility::Visible : winrt_wux::Visibility::Collapsed);
    }
    if (m_gameView) {
        m_gameView.Visibility(view == ActiveView::Games ? winrt_wux::Visibility::Visible : winrt_wux::Visibility::Collapsed);
    }

    if (view == ActiveView::Settings) {
        UpdateCustomScrollThumb();
        if (m_openSettingsSb) m_openSettingsSb.Begin();
    } else if (view == ActiveView::Games) {
        RefreshGameProtectionUi(m_config.maximizeGameRam, true);
        UpdateGameScrollThumb();
        if (m_openGamesSb) m_openGamesSb.Begin();
    } else {
        if (m_openMainSb) m_openMainSb.Begin();
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
            if (m_activeView == ActiveView::Main && m_startupSloganSb) {
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

    if (m_usageRatioText) m_usageRatioText.Text(winrt::hstring(usedStr + L" / " + totalStr));
    if (m_inUseText)      m_inUseText.Text(winrt::hstring(usedStr));
    if (m_availableText)  m_availableText.Text(winrt::hstring(availStr));
    if (m_cachedText)     m_cachedText.Text(winrt::hstring(cacheStr));

    m_targetPercent = static_cast<double>(snap.usagePercent);
    if (std::fabs(m_displayedPercent - m_targetPercent) > 0.25) {
        SetTimer(m_hwnd, IDT_ARC_ANIM, kArcAnimMs, nullptr);
    } else {
        UpdateGaugeVisuals(m_targetPercent);
    }

    UpdateTrayIcon(snap, false);

    ++m_gameBoostTickDivider;
    const bool applyBoostThisTick = m_config.maximizeGameRam && ((m_gameBoostTickDivider % 3) == 1);
    RefreshGameProtectionUi(applyBoostThisTick, false);

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
        _snwprintf_s(pctBuf, _TRUNCATE, L"%u%%", static_cast<unsigned>(std::lround(clamped)));
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
    KillTimer(m_hwnd, IDT_TOAST_COMPLETE);
    m_pendingToastResult.reset();

    if (m_purgePulseSb) {
        m_purgePulseSb.Begin();
    }
    if (m_purgeBtn) m_purgeBtn.IsEnabled(false);
    if (m_purgeBtnText) m_purgeBtnText.Text(L"Purging...");
    if (m_purgeStatusText) {
        m_purgeStatusText.Text(fromAuto ? L"Auto-purging..." : L"Purging...");
        m_purgeStatusText.Foreground(winrt_med::SolidColorBrush(RgbColor(113, 113, 122)));
    }

    if (m_config.showNotifications) {
        m_toastPurgeStartTick = GetTickCount64();
        m_toastWindow.ShowPurging(m_lastSnapshot, m_lastGameStatus);
    }

    PurgeOptions opts{};
    opts.workingSets     = m_config.purgeWorkingSets;
    opts.systemCache     = m_config.purgeSystemCache;
    opts.standbyList     = m_config.purgeStandbyList;
    opts.modifiedList    = m_config.purgeModifiedList;
    opts.maximizeGameRam = m_config.maximizeGameRam;
    opts.protectedGames  = m_config.protectedGames;

    HWND targetHwnd = m_hwnd;
    std::thread([targetHwnd, opts]() {
        auto* result = new PurgeResult(MemoryEngine::Purge(opts));
        if (!PostMessageW(targetHwnd, WM_APP_PURGE_DONE, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void AppWindow::AddProtectedGameExe(const std::wstring& exeNameOrPath) {
    std::wstring clean = NormalizeExeFilename(exeNameOrPath);
    if (clean.empty()) return;

    for (const auto& existing : m_config.protectedGames) {
        if (_wcsicmp(existing.c_str(), clean.c_str()) == 0) {
            if (m_gameActionStatusText) {
                m_gameActionStatusText.Text(winrt::hstring(clean + L" is already in Game Shield."));
            }
            return;
        }
    }

    m_config.protectedGames.push_back(clean);
    ConfigManager::Save(m_config);
    MemoryEngine::EnableAvailablePrivileges();
    RefreshGameProtectionUi(m_config.maximizeGameRam, true);

    if (m_gameActionStatusText) {
        m_gameActionStatusText.Text(winrt::hstring(L"Protected: " + clean + L" (excluded from purge & RAM prioritized)."));
    }
}

void AppWindow::RemoveProtectedGameExe(const std::wstring& exeName) {
    auto it = std::remove_if(m_config.protectedGames.begin(), m_config.protectedGames.end(),
        [&](const std::wstring& item) {
            return _wcsicmp(item.c_str(), exeName.c_str()) == 0;
        });
    if (it == m_config.protectedGames.end()) return;
    m_config.protectedGames.erase(it, m_config.protectedGames.end());
    ConfigManager::Save(m_config);
    RefreshGameProtectionUi(false, true);

    if (m_gameActionStatusText) {
        m_gameActionStatusText.Text(winrt::hstring(L"Removed " + exeName + L" from Game Shield."));
    }
}

void AppWindow::BrowseForGameExe() {
    wchar_t fileBuf[MAX_PATH * 2]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Game Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = ARRAYSIZE(fileBuf);
    ofn.lpstrTitle = L"Select Game Executable to Protect";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        AddProtectedGameExe(fileBuf);
    }
}

void AppWindow::RefreshGameProtectionUi(bool applyBoostNow, bool rebuildLists) {
    m_lastGameStatus = MemoryEngine::QueryAndApplyGameProtection(m_config.protectedGames, applyBoostNow);

    if (m_gameShieldTitleText && m_gameShieldSubText && m_gameShieldBadgeBorder && m_gameShieldBadgeText) {
        if (m_lastGameStatus.anyRunning) {
            m_gameShieldTitleText.Text(winrt::hstring(m_lastGameStatus.primaryGameName + L" Protected"));
            const std::wstring sub = MemoryEngine::FormatBytes(m_lastGameStatus.combinedWorkingSetBytes) +
                (m_config.maximizeGameRam ? L" • Max RAM & Excluded" : L" • Purge Excluded");
            m_gameShieldSubText.Text(winrt::hstring(sub));
            m_gameShieldBadgeBorder.Background(winrt_med::SolidColorBrush(RgbColor(109, 40, 217)));
            m_gameShieldBadgeText.Text(L"ACTIVE");
            m_gameShieldBadgeText.Foreground(winrt_med::SolidColorBrush(RgbColor(255, 255, 255)));
        } else if (!m_config.protectedGames.empty()) {
            wchar_t titleBuf[64]{};
            _snwprintf_s(titleBuf, _TRUNCATE, L"Game Shield (%zu)", m_config.protectedGames.size());
            m_gameShieldTitleText.Text(winrt::hstring(titleBuf));
            m_gameShieldSubText.Text(winrt::hstring(L"Standby: " + m_config.protectedGames.front() +
                (m_config.protectedGames.size() > 1 ? L" + more" : L"")));
            m_gameShieldBadgeBorder.Background(winrt_med::SolidColorBrush(RgbColor(243, 238, 255)));
            m_gameShieldBadgeText.Text(L"Armed");
            m_gameShieldBadgeText.Foreground(winrt_med::SolidColorBrush(RgbColor(109, 40, 217)));
        } else {
            m_gameShieldTitleText.Text(L"Game Shield");
            m_gameShieldSubText.Text(L"Select game to skip purge");
            m_gameShieldBadgeBorder.Background(winrt_med::SolidColorBrush(RgbColor(243, 238, 255)));
            m_gameShieldBadgeText.Text(L"Select");
            m_gameShieldBadgeText.Foreground(winrt_med::SolidColorBrush(RgbColor(109, 40, 217)));
        }
    }

    if (rebuildLists) {
        RebuildGameListsUi(MemoryEngine::ListRunningProcesses());
    }
}

void AppWindow::RebuildGameListsUi(const std::vector<RunningProcessInfo>& runningProcs) {
    if (m_noGamesEmptyCard) {
        m_noGamesEmptyCard.Visibility(m_config.protectedGames.empty()
            ? winrt_wux::Visibility::Visible
            : winrt_wux::Visibility::Collapsed);
    }

    auto findRunningInfo = [&](const std::wstring& exe) -> const RunningProcessInfo* {
        for (const auto& rp : runningProcs) {
            if (_wcsicmp(rp.exeName.c_str(), exe.c_str()) == 0) {
                return &rp;
            }
        }
        return nullptr;
    };

    if (m_protectedGamesList) {
        m_protectedGamesList.Children().Clear();
        for (const auto& exe : m_config.protectedGames) {
            const RunningProcessInfo* live = findRunningInfo(exe);

            winrt_ctrl::Border card;
            card.Background(winrt_med::SolidColorBrush(live ? RgbColor(245, 243, 255) : RgbColor(250, 250, 252)));
            card.BorderBrush(winrt_med::SolidColorBrush(live ? RgbColor(216, 180, 254) : RgbColor(228, 228, 231)));
            card.BorderThickness(winrt_wux::ThicknessHelper::FromUniformLength(1.0));
            card.CornerRadius(winrt_wux::CornerRadiusHelper::FromUniformRadius(7.0));
            card.Padding(winrt_wux::ThicknessHelper::FromLengths(10.0, 6.0, 6.0, 6.0));

            winrt_ctrl::Grid row;
            winrt_ctrl::ColumnDefinition col0;
            col0.Width(winrt_wux::GridLengthHelper::FromValueAndType(1.0, winrt_wux::GridUnitType::Star));
            winrt_ctrl::ColumnDefinition col1;
            col1.Width(winrt_wux::GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(col0);
            row.ColumnDefinitions().Append(col1);

            winrt_ctrl::StackPanel info;
            info.VerticalAlignment(winrt_wux::VerticalAlignment::Center);

            winrt_ctrl::TextBlock nameTxt;
            nameTxt.Text(winrt::hstring(exe));
            nameTxt.FontSize(12.0);
            nameTxt.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            nameTxt.Foreground(winrt_med::SolidColorBrush(RgbColor(24, 24, 27)));
            nameTxt.TextTrimming(winrt_wux::TextTrimming::CharacterEllipsis);

            winrt_ctrl::TextBlock subTxt;
            subTxt.FontSize(10.5);
            if (live) {
                subTxt.Text(winrt::hstring(L"Running • " + MemoryEngine::FormatBytes(live->totalWorkingSetBytes) +
                    (m_config.maximizeGameRam ? L" (Maximized)" : L" (Protected)")));
                subTxt.Foreground(winrt_med::SolidColorBrush(RgbColor(109, 40, 217)));
            } else {
                subTxt.Text(L"Not running • Protected when launched");
                subTxt.Foreground(winrt_med::SolidColorBrush(RgbColor(113, 113, 122)));
            }

            info.Children().Append(nameTxt);
            info.Children().Append(subTxt);
            winrt_ctrl::Grid::SetColumn(info, 0);

            winrt_ctrl::Button removeBtn;
            removeBtn.Content(winrt::box_value(L"Remove"));
            removeBtn.FontSize(10.5);
            removeBtn.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            removeBtn.Padding(winrt_wux::ThicknessHelper::FromLengths(8.0, 3.0, 8.0, 3.0));
            removeBtn.Background(winrt_med::SolidColorBrush(RgbColor(255, 255, 255)));
            removeBtn.BorderBrush(winrt_med::SolidColorBrush(RgbColor(228, 228, 231)));
            removeBtn.BorderThickness(winrt_wux::ThicknessHelper::FromUniformLength(1.0));
            removeBtn.Foreground(winrt_med::SolidColorBrush(RgbColor(185, 28, 28)));
            removeBtn.CornerRadius(winrt_wux::CornerRadiusHelper::FromUniformRadius(5.0));
            removeBtn.VerticalAlignment(winrt_wux::VerticalAlignment::Center);
            winrt_wux::Automation::AutomationProperties::SetName(removeBtn, winrt::hstring(L"Remove " + exe));
            removeBtn.Click([this, exe](auto&&, auto&&) {
                RemoveProtectedGameExe(exe);
            });
            winrt_ctrl::Grid::SetColumn(removeBtn, 1);

            row.Children().Append(info);
            row.Children().Append(removeBtn);
            card.Child(row);
            m_protectedGamesList.Children().Append(card);
        }
    }

    if (m_runningProcessesList) {
        m_runningProcessesList.Children().Clear();
        int shownCount = 0;
        for (const auto& proc : runningProcs) {
            bool alreadyProtected = false;
            for (const auto& pg : m_config.protectedGames) {
                if (_wcsicmp(pg.c_str(), proc.exeName.c_str()) == 0) {
                    alreadyProtected = true;
                    break;
                }
            }
            if (alreadyProtected) continue;
            if (shownCount >= 18) break;
            ++shownCount;

            winrt_ctrl::Border card;
            card.Background(winrt_med::SolidColorBrush(RgbColor(255, 255, 255)));
            card.BorderBrush(winrt_med::SolidColorBrush(RgbColor(239, 239, 244)));
            card.BorderThickness(winrt_wux::ThicknessHelper::FromUniformLength(1.0));
            card.CornerRadius(winrt_wux::CornerRadiusHelper::FromUniformRadius(7.0));
            card.Padding(winrt_wux::ThicknessHelper::FromLengths(10.0, 5.0, 6.0, 5.0));

            winrt_ctrl::Grid row;
            winrt_ctrl::ColumnDefinition col0;
            col0.Width(winrt_wux::GridLengthHelper::FromValueAndType(1.0, winrt_wux::GridUnitType::Star));
            winrt_ctrl::ColumnDefinition col1;
            col1.Width(winrt_wux::GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(col0);
            row.ColumnDefinitions().Append(col1);

            winrt_ctrl::StackPanel info;
            info.VerticalAlignment(winrt_wux::VerticalAlignment::Center);

            winrt_ctrl::TextBlock nameTxt;
            nameTxt.Text(winrt::hstring(proc.exeName));
            nameTxt.FontSize(11.5);
            nameTxt.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            nameTxt.Foreground(winrt_med::SolidColorBrush(RgbColor(24, 24, 27)));
            nameTxt.TextTrimming(winrt_wux::TextTrimming::CharacterEllipsis);

            winrt_ctrl::TextBlock ramTxt;
            wchar_t rbuf[64]{};
            _snwprintf_s(rbuf, _TRUNCATE, L"%s%s",
                         MemoryEngine::FormatBytes(proc.totalWorkingSetBytes).c_str(),
                         proc.hasWindow ? L" • App Window" : L" • Background");
            ramTxt.Text(winrt::hstring(rbuf));
            ramTxt.FontSize(10.0);
            ramTxt.Foreground(winrt_med::SolidColorBrush(RgbColor(113, 113, 122)));

            info.Children().Append(nameTxt);
            info.Children().Append(ramTxt);
            winrt_ctrl::Grid::SetColumn(info, 0);

            winrt_ctrl::Button addBtn;
            addBtn.Content(winrt::box_value(L"+ Protect"));
            addBtn.FontSize(10.5);
            addBtn.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            addBtn.Padding(winrt_wux::ThicknessHelper::FromLengths(8.0, 3.0, 8.0, 3.0));
            addBtn.Background(winrt_med::SolidColorBrush(RgbColor(243, 238, 255)));
            addBtn.Foreground(winrt_med::SolidColorBrush(RgbColor(109, 40, 217)));
            addBtn.BorderThickness(winrt_wux::ThicknessHelper::FromUniformLength(0.0));
            addBtn.CornerRadius(winrt_wux::CornerRadiusHelper::FromUniformRadius(5.0));
            addBtn.VerticalAlignment(winrt_wux::VerticalAlignment::Center);
            const std::wstring exeCopy = proc.exeName;
            winrt_wux::Automation::AutomationProperties::SetName(addBtn, winrt::hstring(L"Protect " + exeCopy));
            addBtn.Click([this, exeCopy](auto&&, auto&&) {
                AddProtectedGameExe(exeCopy);
            });
            winrt_ctrl::Grid::SetColumn(addBtn, 1);

            row.Children().Append(info);
            row.Children().Append(addBtn);
            card.Child(row);
            m_runningProcessesList.Children().Append(card);
        }
    }

    UpdateGameScrollThumb();
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
    wcscpy_s(nid.szTip, L"VRam V2.0");

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
    _snwprintf_s(nid.szTip, _TRUNCATE,
                 L"VRam V2.0 - RAM: %u%% (%s / %s)",
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

void AppWindow::ShowTrayContextMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    const bool visible = IsWindowVisible(m_hwnd) != FALSE;
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, visible ? L"Hide to Tray" : L"Show VRam V2.0");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_PURGE, L"Purge Memory Now");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_GAMES, L"Game Shield");
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
