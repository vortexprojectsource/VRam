#include "AppWindow.hpp"
#include "Win32Compat.hpp"
#include "../resources/resource.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace vram {

namespace {

constexpr wchar_t kWindowClassName[] = L"VRam_V2_HostWindow";
constexpr int kClientDipWidth  = 368;
constexpr int kClientDipHeight = 524;

constexpr UINT_PTR IDT_POLL_MEMORY    = 1001;
constexpr UINT_PTR IDT_UI_ANIM        = 1002;
constexpr UINT_PTR IDT_WINDOW_FADE    = 1003;
constexpr UINT_PTR IDT_STATUS_RESET   = 1004;
constexpr UINT_PTR IDT_TOAST_COMPLETE = 1005;
constexpr UINT_PTR IDT_TOOLTIP_CHECK  = 1006;

constexpr UINT     kPollIntervalMs    = 2000;
constexpr UINT     kUiAnimMs          = 16;
constexpr UINT     kFadeTimerMs       = 12;
constexpr UINT     kStatusResetMs     = 4000;
constexpr uint64_t kToastMinRunMs     = 550;
constexpr uint64_t kTooltipDelayMs    = 450;

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
constexpr float kPi = 3.14159265358979323846f;

std::wstring NormalizeExeFilename(const std::wstring& pathOrName) {
    if (pathOrName.empty()) return {};
    const wchar_t* leaf = PathFindFileNameW(pathOrName.c_str());
    return (leaf && *leaf) ? std::wstring(leaf) : pathOrName;
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

float SmoothApproach(float current, float target, float factor, bool& moving) {
    const float diff = target - current;
    if (std::fabs(diff) <= 0.004f) {
        return target;
    }
    moving = true;
    return current + diff * factor;
}

D2D1_COLOR_F LerpColor(uint32_t hexA, uint32_t hexB, float t) {
    const float ct = std::clamp(t, 0.0f, 1.0f);
    const float rA = static_cast<float>((hexA >> 16) & 0xFF) / 255.0f;
    const float gA = static_cast<float>((hexA >> 8) & 0xFF) / 255.0f;
    const float bA = static_cast<float>(hexA & 0xFF) / 255.0f;
    const float rB = static_cast<float>((hexB >> 16) & 0xFF) / 255.0f;
    const float gB = static_cast<float>((hexB >> 8) & 0xFF) / 255.0f;
    const float bB = static_cast<float>(hexB & 0xFF) / 255.0f;
    return D2D1::ColorF(rA + (rB - rA) * ct, gA + (gB - gA) * ct, bA + (bB - bA) * ct, 1.0f);
}

}

AppWindow::AppWindow(HINSTANCE hInst, bool startMinimized)
    : m_hInst(hInst)
    , m_startMinimized(startMinimized)
    , m_isElevated(MemoryEngine::IsElevated()) {
    m_config = ConfigManager::Load();
    m_lastPurgeTickMs = GetTickCount64();
    SyncToggleTargetsFromConfig(true);
}

AppWindow::~AppWindow() {
    if (m_appIconLarge) DestroyIcon(m_appIconLarge);
    if (m_appIconSmall) DestroyIcon(m_appIconSmall);
    if (m_currentTrayIcon) DestroyIcon(m_currentTrayIcon);
}

bool AppWindow::Create() {
    InitializeDeviceIndependentGraphics();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = m_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClassName;

    m_dpi = compat::GetDpiForSystemCompat();
    const int cxBig = compat::GetSystemMetricsForDpiCompat(SM_CXICON, m_dpi);
    const int cyBig = compat::GetSystemMetricsForDpiCompat(SM_CYICON, m_dpi);
    const int cxSm  = compat::GetSystemMetricsForDpiCompat(SM_CXSMICON, m_dpi);
    const int cySm  = compat::GetSystemMetricsForDpiCompat(SM_CYSMICON, m_dpi);

    m_appIconLarge = static_cast<HICON>(LoadImageW(
        m_hInst, MAKEINTRESOURCEW(IDI_VRAM_APP), IMAGE_ICON, cxBig, cyBig, LR_DEFAULTCOLOR));
    m_appIconSmall = static_cast<HICON>(LoadImageW(
        m_hInst, MAKEINTRESOURCEW(IDI_VRAM_APP), IMAGE_ICON, cxSm, cySm, LR_DEFAULTCOLOR));
    wc.hIcon = m_appIconLarge;
    wc.hIconSm = m_appIconSmall;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;

    const int winW = compat::ScaleDip(kClientDipWidth, m_dpi);
    const int winH = compat::ScaleDip(kClientDipHeight, m_dpi);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int posX = workArea.left + ((workArea.right - workArea.left) - winW) / 2;
    const int posY = workArea.top + ((workArea.bottom - workArea.top) - winH) / 2;

    m_hwnd = CreateWindowExW(
        exStyle,
        kWindowClassName,
        L"VRam V2.1",
        style,
        posX, posY, winW, winH,
        nullptr, nullptr, m_hInst, this
    );
    if (!m_hwnd) {
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
        m_sloganAnimT = 0.0f;
        m_sloganAnimTarget = 1.0f;
        StartWindowFade(FadeMode::In);
        StartUiAnimationLoop();
    }

    return true;
}

bool AppWindow::PreTranslateMessage(MSG*) {
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

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_PRINTCLIENT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        if (hdc) {
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            PaintToHdc(hdc, rc.right - rc.left, rc.bottom - rc.top);
        }
        return 0;
    }

    case WM_SIZE:
        OnSize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, m_hoverControl.valid() ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

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
    m_dpi = compat::GetDpiForWindowCompat(m_hwnd);
    InitTrayIcon();
    RefreshMemorySnapshot(false);
    RefreshGameProtectionUi(m_config.maximizeGameRam, true);
    m_displayedPercent = m_targetPercent;
    UpdateIdlePurgeStatusText();

    SetTimer(m_hwnd, IDT_POLL_MEMORY, kPollIntervalMs, nullptr);
}

void AppWindow::OnDestroy() {
    KillTimer(m_hwnd, IDT_POLL_MEMORY);
    KillTimer(m_hwnd, IDT_UI_ANIM);
    KillTimer(m_hwnd, IDT_WINDOW_FADE);
    KillTimer(m_hwnd, IDT_STATUS_RESET);
    KillTimer(m_hwnd, IDT_TOAST_COMPLETE);
    KillTimer(m_hwnd, IDT_TOOLTIP_CHECK);
    m_animTimerActive = false;
    m_pendingToastResult.reset();
    m_toastWindow.Destroy();
    RemoveTrayIcon();
    DiscardRenderTarget();
}

void AppWindow::OnSize(UINT, UINT) {
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AppWindow::OnDpiChanged(UINT newDpi, const RECT* suggestedRect) {
    m_dpi = (newDpi > 0) ? newDpi : 96u;
    if (suggestedRect) {
        SetWindowPos(m_hwnd, nullptr,
                     suggestedRect->left, suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (m_rt) {
        m_rt->SetDpi(static_cast<float>(m_dpi), static_cast<float>(m_dpi));
    }
    m_logoPbgra.clear();
    LoadLogoBitmap();
    UpdateTrayIcon(m_lastSnapshot, true);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AppWindow::OnTimer(UINT_PTR timerId) {
    if (timerId == IDT_POLL_MEMORY) {
        RefreshMemorySnapshot(true);
    } else if (timerId == IDT_UI_ANIM) {
        if (!AdvanceAnimations()) {
            KillTimer(m_hwnd, IDT_UI_ANIM);
            m_animTimerActive = false;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    } else if (timerId == IDT_TOAST_COMPLETE) {
        KillTimer(m_hwnd, IDT_TOAST_COMPLETE);
        if (m_pendingToastResult) {
            if (m_config.showNotifications) {
                m_toastWindow.ShowCompleted(*m_pendingToastResult);
            }
            m_pendingToastResult.reset();
        }
    } else if (timerId == IDT_TOOLTIP_CHECK) {
        if (!m_tooltipVisible && m_hoverControl.valid() && !m_hoverTooltipText.empty() &&
            !m_isDraggingWindow && (GetTickCount64() - m_hoverStartTick) >= kTooltipDelayMs) {
            m_tooltipVisible = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
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
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void AppWindow::OnMouseMove(int px, int py) {
    if (!m_trackingMouseLeave) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
        m_trackingMouseLeave = (TrackMouseEvent(&tme) != FALSE);
    }

    const float scale = (m_dpi > 0) ? (96.0f / static_cast<float>(m_dpi)) : 1.0f;
    const float dipX = static_cast<float>(px) * scale;
    const float dipY = static_cast<float>(py) * scale;

    if (m_isDraggingWindow) {
        if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
            m_isDraggingWindow = false;
            ReleaseCapture();
            return;
        }
        POINT cur{};
        if (GetCursorPos(&cur)) {
            SetWindowPos(m_hwnd, nullptr,
                         m_dragStartWin.x + (cur.x - m_dragStartCursor.x),
                         m_dragStartWin.y + (cur.y - m_dragStartCursor.y),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return;
    }

    if (m_draggingScrollThumb) {
        const size_t idx = static_cast<size_t>(m_activeView);
        const float maxOff = m_scrollMaxOffset[idx];
        const float trackH = m_activeScrollTrack.bottom - m_activeScrollTrack.top;
        const float contentH = m_scrollContentHeight[idx];
        if (maxOff > 0.0f && trackH > 0.0f && contentH > m_scrollViewHeight) {
            const float thumbH = std::max(28.0f, trackH * (m_scrollViewHeight / contentH));
            const float travel = std::max(1.0f, trackH - thumbH);
            const float deltaY = dipY - m_scrollDragStartDipY;
            m_scrollOffset[idx] = std::clamp(m_scrollDragStartOffset + deltaY * (maxOff / travel), 0.0f, maxOff);
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    std::wstring tip;
    D2D1_RECT_F tipAnchor{};
    const ControlTarget nextHover = HitTest(dipX, dipY, &tip, &tipAnchor);
    if (nextHover != m_hoverControl) {
        const bool wasSettings = (m_hoverControl.id == ControlId::SettingsBtn);
        const bool isSettings = (nextHover.id == ControlId::SettingsBtn);
        m_hoverControl = nextHover;
        m_hoverTooltipText = tip;
        m_hoverTooltipAnchor = tipAnchor;
        m_hoverStartTick = GetTickCount64();
        m_tooltipVisible = false;
        KillTimer(m_hwnd, IDT_TOOLTIP_CHECK);
        if (!tip.empty()) {
            SetTimer(m_hwnd, IDT_TOOLTIP_CHECK, 120, nullptr);
        }
        if (wasSettings != isSettings) {
            StartUiAnimationLoop();
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void AppWindow::OnMouseLeave() {
    m_trackingMouseLeave = false;
    KillTimer(m_hwnd, IDT_TOOLTIP_CHECK);
    if (m_hoverControl.valid() || m_tooltipVisible) {
        m_hoverControl = {};
        m_tooltipVisible = false;
        m_hoverTooltipText.clear();
        StartUiAnimationLoop();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void AppWindow::OnLButtonDown(int px, int py) {
    SetFocus(m_hwnd);
    KillTimer(m_hwnd, IDT_TOOLTIP_CHECK);
    if (m_tooltipVisible) {
        m_tooltipVisible = false;
    }

    const float scale = (m_dpi > 0) ? (96.0f / static_cast<float>(m_dpi)) : 1.0f;
    const float dipX = static_cast<float>(px) * scale;
    const float dipY = static_cast<float>(py) * scale;

    const ControlTarget hit = HitTest(dipX, dipY);
    if (hit.id == ControlId::ScrollThumb) {
        m_draggingScrollThumb = true;
        m_scrollDragStartDipY = dipY;
        m_scrollDragStartOffset = m_scrollOffset[static_cast<size_t>(m_activeView)];
        m_pressedControl = hit;
        SetCapture(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    if (hit.valid()) {
        m_pressedControl = hit;
        SetCapture(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    const bool inDragArea = (dipY < 48.0f) ||
        (m_activeView == ActiveView::Main && dipY >= 48.0f) ||
        (m_activeView != ActiveView::Main && dipY < 92.0f);
    if (inDragArea) {
        RECT winRc{};
        if (GetWindowRect(m_hwnd, &winRc) && GetCursorPos(&m_dragStartCursor)) {
            m_dragStartWin.x = winRc.left;
            m_dragStartWin.y = winRc.top;
            m_isDraggingWindow = true;
            SetCapture(m_hwnd);
        }
    }
}

void AppWindow::OnLButtonUp(int px, int py) {
    if (m_isDraggingWindow) {
        m_isDraggingWindow = false;
        ReleaseCapture();
        return;
    }

    if (m_draggingScrollThumb) {
        m_draggingScrollThumb = false;
        m_pressedControl = {};
        ReleaseCapture();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    const ControlTarget pressed = m_pressedControl;
    m_pressedControl = {};
    if (GetCapture() == m_hwnd) {
        ReleaseCapture();
    }

    const float scale = (m_dpi > 0) ? (96.0f / static_cast<float>(m_dpi)) : 1.0f;
    const float dipX = static_cast<float>(px) * scale;
    const float dipY = static_cast<float>(py) * scale;

    if (pressed.valid()) {
        const ControlTarget released = HitTest(dipX, dipY);
        if (released == pressed) {
            ActivateControl(released);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void AppWindow::OnMouseWheel(short wheelDelta) {
    if (m_activeView == ActiveView::Main) return;
    const float notches = static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA);
    ScrollBy(-notches * 36.0f);
}

void AppWindow::OnKeyDown(WPARAM vk) {
    if (vk == VK_ESCAPE && m_activeView != ActiveView::Main) {
        SwitchToView(ActiveView::Main);
        return;
    }
    if (m_activeView == ActiveView::Main) return;
    if (vk == VK_DOWN) ScrollBy(36.0f);
    else if (vk == VK_UP) ScrollBy(-36.0f);
    else if (vk == VK_NEXT) ScrollBy(180.0f);
    else if (vk == VK_PRIOR) ScrollBy(-180.0f);
    else if (vk == VK_HOME) ScrollBy(-10000.0f);
    else if (vk == VK_END) ScrollBy(10000.0f);
}

bool AppWindow::InitializeDeviceIndependentGraphics() {
    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }

    m_icons.Initialize(m_d2dFactory.Get());

    if (!m_roundCapStroke) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        m_d2dFactory->CreateStrokeStyle(&sp, nullptr, 0, &m_roundCapStroke);
    }

    auto makeFmt = [&](ComPtr<IDWriteTextFormat>& dst, float pt, DWRITE_FONT_WEIGHT weight,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                       bool ellipsis = false, bool wrap = false) {
        if (dst) return;
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, pt, L"en-us", &dst);
        if (dst) {
            dst->SetTextAlignment(hAlign);
            dst->SetParagraphAlignment(vAlign);
            dst->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            if (ellipsis) {
                ComPtr<IDWriteInlineObject> trimSign;
                if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(dst.Get(), &trimSign)) && trimSign) {
                    DWRITE_TRIMMING trimOpt{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    dst->SetTrimming(&trimOpt, trimSign.Get());
                }
            }
        }
    };

    makeFmt(m_fmtPercent,       36.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    makeFmt(m_fmtHeaderTitle,   14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    makeFmt(m_fmtBtnLarge,      13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    makeFmt(m_fmtMetricVal,     12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    makeFmt(m_fmtMetricLbl,      9.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    makeFmt(m_fmtRowTitle,      12.5f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    makeFmt(m_fmtBody,          11.5f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    makeFmt(m_fmtBodySemi,      11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    makeFmt(m_fmtSub,           10.5f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    makeFmt(m_fmtSection,       10.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    makeFmt(m_fmtSmallCenter,   11.0f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
    makeFmt(m_fmtSmallBoldCenter, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);

    return true;
}

bool AppWindow::EnsureRenderTarget(int widthPx, int heightPx) {
    if (!m_hwnd || widthPx <= 0 || heightPx <= 0 || !InitializeDeviceIndependentGraphics()) {
        return false;
    }

    if (!m_memDc || !m_memBmp || m_memW != widthPx || m_memH != heightPx) {
        if (m_memDc) {
            if (m_oldMemBmp) {
                SelectObject(m_memDc, m_oldMemBmp);
                m_oldMemBmp = nullptr;
            }
            if (m_memBmp) {
                DeleteObject(m_memBmp);
                m_memBmp = nullptr;
            }
            DeleteDC(m_memDc);
            m_memDc = nullptr;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = widthPx;
        bi.bmiHeader.biHeight = -heightPx;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screenDc = GetDC(nullptr);
        m_memDc = CreateCompatibleDC(screenDc);
        void* bits = nullptr;
        m_memBmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (screenDc) ReleaseDC(nullptr, screenDc);
        if (!m_memDc || !m_memBmp) {
            DiscardRenderTarget();
            return false;
        }
        m_oldMemBmp = SelectObject(m_memDc, m_memBmp);
        m_memW = widthPx;
        m_memH = heightPx;
    }

    if (!m_rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(m_dpi),
            static_cast<float>(m_dpi)
        );
        HRESULT hr = m_d2dFactory->CreateDCRenderTarget(&props, &m_rt);
        if (FAILED(hr) || !m_rt) {
            props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
            hr = m_d2dFactory->CreateDCRenderTarget(&props, &m_rt);
        }
        if (FAILED(hr) || !m_rt) {
            return false;
        }
        m_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        LoadLogoBitmap();
    }

    m_rt->SetDpi(static_cast<float>(m_dpi), static_cast<float>(m_dpi));
    RECT bindRc{0, 0, widthPx, heightPx};
    return SUCCEEDED(m_rt->BindDC(m_memDc, &bindRc));
}

void AppWindow::DiscardRenderTarget() {
    m_logoBitmap.Reset();
    m_rt.Reset();
    if (m_memDc) {
        if (m_oldMemBmp) {
            SelectObject(m_memDc, m_oldMemBmp);
            m_oldMemBmp = nullptr;
        }
        if (m_memBmp) {
            DeleteObject(m_memBmp);
            m_memBmp = nullptr;
        }
        DeleteDC(m_memDc);
        m_memDc = nullptr;
    }
    m_memW = 0;
    m_memH = 0;
}

void AppWindow::LoadLogoBitmap() {
    m_logoBitmap.Reset();
    if (!m_rt) return;

    if (m_logoPbgra.empty()) {
        auto pngBytes = TrayIconRenderer::LoadLogoPngBytes(m_hInst);
        const UINT decodeH = static_cast<UINT>(std::max(44, compat::ScaleDip(44, m_dpi)));
        compat::DecodePngToPbgra(pngBytes, decodeH, m_logoBmpW, m_logoBmpH, m_logoPbgra);
    }
    if (!m_logoPbgra.empty() && m_logoBmpW > 0 && m_logoBmpH > 0) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        m_rt->CreateBitmap(D2D1::SizeU(m_logoBmpW, m_logoBmpH), m_logoPbgra.data(), m_logoBmpW * 4, bp, &m_logoBitmap);
    }
}

void AppWindow::PaintToHdc(HDC dstHdc, int widthPx, int heightPx) {
    if (!dstHdc || widthPx <= 0 || heightPx <= 0) return;

    if (EnsureRenderTarget(widthPx, heightPx)) {
        m_rt->BeginDraw();
        RenderD2D(m_rt.Get());
        const HRESULT hr = m_rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardRenderTarget();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        BitBlt(dstHdc, 0, 0, widthPx, heightPx, m_memDc, 0, 0, SRCCOPY);
    } else {
        RECT rc{0, 0, widthPx, heightPx};
        RenderGdiFallback(dstHdc, rc);
    }
}

void AppWindow::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(m_hwnd, &ps);
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    PaintToHdc(hdc, rc.right - rc.left, rc.bottom - rc.top);
    EndPaint(m_hwnd, &ps);
}

void AppWindow::RenderGdiFallback(HDC hdc, const RECT& clientRc) {
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &clientRc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(109, 40, 217));
    wchar_t buf[96]{};
    _snwprintf_s(buf, _TRUNCATE, L"VRam V2.1 - RAM: %u%%", m_lastSnapshot.usagePercent);
    RECT txtRc = clientRc;
    DrawTextW(hdc, buf, -1, &txtRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void AppWindow::RenderD2D(ID2D1RenderTarget* rt) {
    m_hitControls.clear();
    const D2D1_SIZE_F sz = rt->GetSize();
    const float w = (sz.width > 10.0f) ? sz.width : static_cast<float>(kClientDipWidth);
    const float h = (sz.height > 10.0f) ? sz.height : static_cast<float>(kClientDipHeight);

    rt->Clear(D2D1::ColorF(0xFFFFFF, 1.0f));

    DrawHeader(rt, w);

    const float ease = 1.0f - std::pow(1.0f - m_viewTransT, 3.0f);
    const float slideX = (m_viewTransT < 1.0f) ? (static_cast<float>(m_viewSlideDir) * 24.0f * (1.0f - ease)) : 0.0f;
    const float opacity = (m_viewTransT < 1.0f) ? ease : 1.0f;

    if (m_activeView == ActiveView::Main) {
        DrawMainView(rt, w, h, slideX, opacity);
    } else if (m_activeView == ActiveView::Games) {
        DrawGameView(rt, w, h, slideX, opacity);
    } else {
        DrawSettingsView(rt, w, h, slideX, opacity);
    }

    DrawTooltipOverlay(rt, w, h);

    // Subtle 1px outer frame border for clean edges on Windows 10 & Windows 11
    ComPtr<ID2D1SolidColorBrush> brFrame;
    rt->CreateSolidColorBrush(D2D1::ColorF(0xE4E4E7, 1.0f), &brFrame);
    if (brFrame) {
        rt->DrawRectangle(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), brFrame.Get(), 1.0f);
    }
}

void AppWindow::DrawHeader(ID2D1RenderTarget* rt, float w) {
    ComPtr<ID2D1SolidColorBrush> brDivider;
    ComPtr<ID2D1SolidColorBrush> brInk;
    rt->CreateSolidColorBrush(D2D1::ColorF(0xF1F1F5, 1.0f), &brDivider);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x18181B, 1.0f), &brInk);

    if (m_logoBitmap) {
        const D2D1_SIZE_F bmpSz = m_logoBitmap->GetSize();
        const float drawH = 22.0f;
        const float drawW = (bmpSz.height > 0.0f) ? std::min(46.0f, drawH * bmpSz.width / bmpSz.height) : 42.0f;
        rt->DrawBitmap(m_logoBitmap.Get(), D2D1::RectF(20.0f, 15.0f, 20.0f + drawW, 15.0f + drawH));
    } else if (brInk) {
        m_icons.Draw(rt, VectorIconId::Lightning, D2D1::RectF(20.0f, 18.0f, 36.0f, 34.0f), brInk.Get(), 1.8f);
    }

    const D2D1_RECT_F minRc = D2D1::RectF(w - 78.0f, 11.0f, w - 48.0f, 41.0f);
    const D2D1_RECT_F clsRc = D2D1::RectF(w - 44.0f, 11.0f, w - 14.0f, 41.0f);

    DrawIconButton(rt, minRc, ControlId::WinMinimize, VectorIconId::Minimize, 11.0f,
                   D2D1::ColorF(0x3F3F46), D2D1::ColorF(0xF1F1F4), D2D1::ColorF(0xE4E4E9),
                   2.2f, 0.0f, L"Minimize");
    DrawIconButton(rt, clsRc, ControlId::WinClose, VectorIconId::Close, 11.0f,
                   D2D1::ColorF(0x3F3F46), D2D1::ColorF(0xFEE2E2), D2D1::ColorF(0xFECACA),
                   2.2f, 0.0f, L"Close");

    if (brDivider) {
        rt->DrawLine(D2D1::Point2F(20.0f, 48.5f), D2D1::Point2F(w - 20.0f, 48.5f), brDivider.Get(), 1.0f);
    }
}

void AppWindow::DrawMainView(ID2D1RenderTarget* rt, float w, float h, float slideX, float opacity) {
    D2D1_MATRIX_3X2_F oldTx{};
    rt->GetTransform(&oldTx);
    if (slideX != 0.0f) {
        rt->SetTransform(D2D1::Matrix3x2F::Translation(slideX, 0.0f) * oldTx);
    }

    ComPtr<ID2D1SolidColorBrush> brTextDark, brTextMed, brTextMuted, brBlack, brDivider;
    ComPtr<ID2D1SolidColorBrush> brPurple, brLavender, brWhite, brPulse;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x27272A, opacity), &brTextDark);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x52525B, opacity), &brTextMed);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x71717A, opacity), &brTextMuted);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x111113, opacity), &brBlack);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xEFEFF4, opacity), &brDivider);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x6D28D9, opacity), &brPurple);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xF3EEFF, opacity), &brLavender);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, opacity), &brWhite);

    // Row 0: Animated staggered slogans
    struct SloganItem { const wchar_t* text; ID2D1SolidColorBrush* brush; float startAt; };
    const SloganItem slogans[3] = {
        { L"Keep RAM clear and responsive.",         brTextDark.Get(),  0.00f },
        { L"Trim idle processes & system cache.",    brTextMed.Get(),   0.18f },
        { L"Protect selected games & maximize RAM.", brTextMuted.Get(), 0.36f }
    };

    float curY = 63.0f;
    for (int i = 0; i < 3; ++i) {
        const float rawT = std::clamp((m_sloganAnimT - slogans[i].startAt) / 0.64f, 0.0f, 1.0f);
        const float e = 1.0f - std::pow(1.0f - rawT, 3.0f);
        const float dy = 6.0f * (1.0f - e);

        ComPtr<ID2D1SolidColorBrush> rowBrush;
        const D2D1_COLOR_F baseC = slogans[i].brush ? slogans[i].brush->GetColor() : D2D1::ColorF(0x27272A);
        rt->CreateSolidColorBrush(D2D1::ColorF(baseC.r, baseC.g, baseC.b, e * opacity), &rowBrush);
        ComPtr<ID2D1SolidColorBrush> iconBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0x71717A, e * opacity), &iconBrush);

        if (iconBrush) {
            m_icons.Draw(rt, VectorIconId::ArrowRight,
                         D2D1::RectF(20.0f, curY + 2.5f + dy, 31.0f, curY + 13.5f + dy), iconBrush.Get(), 2.0f);
        }
        if (rowBrush && m_fmtBody) {
            rt->DrawTextW(slogans[i].text, static_cast<UINT32>(wcslen(slogans[i].text)), m_fmtBody.Get(),
                          D2D1::RectF(39.0f, curY + dy, w - 20.0f, curY + 16.0f + dy), rowBrush.Get());
        }
        curY += 19.0f;
    }

    // Row 1: Center Purple Ring Gauge
    const float cx = w * 0.5f;
    const float cy = 209.0f;
    const float radius = 70.0f;

    if (m_purgeRunning.load()) {
        const float pulseEase = 1.0f - std::pow(1.0f - m_purgePulsePhase, 3.0f);
        const float pr = radius * (1.0f + 0.14f * pulseEase);
        const float popa = 0.55f * (1.0f - pulseEase) * opacity;
        rt->CreateSolidColorBrush(D2D1::ColorF(0x7C3AED, popa), &brPulse);
        if (brPulse) {
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), pr, pr), brPulse.Get(), 2.0f);
        }
    }

    if (brLavender) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brLavender.Get(), 8.0f);
    }

    const double clampedPct = std::clamp(m_displayedPercent, 0.0, 100.0);
    if (clampedPct >= 0.5) {
        const double sweepDeg = std::min(359.95, (clampedPct / 100.0) * 360.0);
        const double endRad = (sweepDeg - 90.0) * (static_cast<double>(kPi) / 180.0);

        const D2D1_POINT_2F startPt{cx, cy - radius};
        const D2D1_POINT_2F endPt{
            cx + radius * static_cast<float>(std::cos(endRad)),
            cy + radius * static_cast<float>(std::sin(endRad))
        };

        ComPtr<ID2D1PathGeometry> arcGeo;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(&arcGeo)) && arcGeo &&
            SUCCEEDED(arcGeo->Open(&sink)) && sink) {
            sink->BeginFigure(startPt, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(
                endPt,
                D2D1::SizeF(radius, radius),
                0.0f,
                D2D1_SWEEP_DIRECTION_CLOCKWISE,
                (sweepDeg > 180.0) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();

            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, D2D1::ColorF(0x6D28D9, opacity)},
                {1.0f, D2D1::ColorF(0x9333EA, opacity)}
            };
            ComPtr<ID2D1GradientStopCollection> stopCol;
            ComPtr<ID2D1LinearGradientBrush> gradBrush;
            if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, &stopCol)) && stopCol &&
                SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(cx - radius, cy - radius),
                        D2D1::Point2F(cx + radius, cy + radius)),
                    stopCol.Get(), &gradBrush)) && gradBrush) {
                rt->DrawGeometry(arcGeo.Get(), gradBrush.Get(), 8.0f, m_roundCapStroke.Get());
            } else if (brPurple) {
                rt->DrawGeometry(arcGeo.Get(), brPurple.Get(), 8.0f, m_roundCapStroke.Get());
            }
        }
    }

    wchar_t pctBuf[16]{};
    _snwprintf_s(pctBuf, _TRUNCATE, L"%u%%", static_cast<unsigned>(std::lround(clampedPct)));
    if (m_fmtPercent && brBlack) {
        rt->DrawTextW(pctBuf, static_cast<UINT32>(wcslen(pctBuf)), m_fmtPercent.Get(),
                      D2D1::RectF(cx - 64.0f, cy - 30.0f, cx + 64.0f, cy + 12.0f), brBlack.Get());
    }

    const std::wstring usedStr  = MemoryEngine::FormatBytes(m_lastSnapshot.usedBytes);
    const std::wstring totalStr = MemoryEngine::FormatBytes(m_lastSnapshot.totalBytes);
    const std::wstring ratioStr = usedStr + L" / " + totalStr;
    if (m_fmtSmallBoldCenter && brTextMuted) {
        rt->DrawTextW(ratioStr.c_str(), static_cast<UINT32>(ratioStr.size()), m_fmtSmallBoldCenter.Get(),
                      D2D1::RectF(cx - 68.0f, cy + 14.0f, cx + 68.0f, cy + 32.0f), brTextMuted.Get());
    }

    // Row 2: 3-Column Metrics strip
    const float stripTop = 301.0f;
    const float stripBot = 347.0f;
    if (brDivider) {
        rt->DrawLine(D2D1::Point2F(20.0f, stripTop + 0.5f), D2D1::Point2F(w - 20.0f, stripTop + 0.5f), brDivider.Get(), 1.0f);
        rt->DrawLine(D2D1::Point2F(20.0f, stripBot - 0.5f), D2D1::Point2F(w - 20.0f, stripBot - 0.5f), brDivider.Get(), 1.0f);
    }

    const float colW = (w - 40.0f) / 3.0f;
    const std::wstring availStr = MemoryEngine::FormatBytes(m_lastSnapshot.availBytes);
    const std::wstring cacheStr = MemoryEngine::FormatBytes(m_lastSnapshot.cachedBytes);
    struct MetricCol { const wchar_t* label; const wchar_t* val; ID2D1SolidColorBrush* valBrush; };
    const MetricCol cols[3] = {
        { L"USED",   usedStr.c_str(),  brTextDark.Get() },
        { L"FREE",   availStr.c_str(), brPurple.Get()   },
        { L"CACHED", cacheStr.c_str(), brTextDark.Get() }
    };

    for (int i = 0; i < 3; ++i) {
        const float cx0 = 20.0f + colW * static_cast<float>(i);
        const float cx1 = cx0 + colW;
        if (i > 0 && brDivider) {
            rt->DrawLine(D2D1::Point2F(cx0, stripTop + 8.0f), D2D1::Point2F(cx0, stripBot - 8.0f), brDivider.Get(), 1.0f);
        }
        if (m_fmtMetricLbl && brTextMuted) {
            rt->DrawTextW(cols[i].label, static_cast<UINT32>(wcslen(cols[i].label)), m_fmtMetricLbl.Get(),
                          D2D1::RectF(cx0 + 2.0f, stripTop + 7.0f, cx1 - 2.0f, stripTop + 20.0f), brTextMuted.Get());
        }
        if (m_fmtMetricVal && cols[i].valBrush) {
            rt->DrawTextW(cols[i].val, static_cast<UINT32>(wcslen(cols[i].val)), m_fmtMetricVal.Get(),
                          D2D1::RectF(cx0 + 2.0f, stripTop + 22.0f, cx1 - 2.0f, stripBot - 5.0f), cols[i].valBrush);
        }
    }

    // Row 3: Game Shield Selector Bar
    D2D1_RECT_F gsRc = D2D1::RectF(20.0f, 357.0f, w - 20.0f, 409.0f);
    RegisterHit(ControlId::GameShieldCard, gsRc);
    const bool gsHover = (m_hoverControl.id == ControlId::GameShieldCard);
    const bool gsPress = (m_pressedControl.id == ControlId::GameShieldCard);
    const D2D1_COLOR_F gsBg  = gsPress ? D2D1::ColorF(0xEDE9FE, opacity) : (gsHover ? D2D1::ColorF(0xF5F3FF, opacity) : D2D1::ColorF(0xFAFAFC, opacity));
    const D2D1_COLOR_F gsBrd = gsPress ? D2D1::ColorF(0xC4B5FD, opacity) : (gsHover ? D2D1::ColorF(0xDDD6FE, opacity) : D2D1::ColorF(0xEAEAF2, opacity));

    if (gsPress) {
        gsRc.left += 1.5f; gsRc.top += 0.5f; gsRc.right -= 1.5f; gsRc.bottom -= 0.5f;
    }

    ComPtr<ID2D1SolidColorBrush> brGsBg, brGsBrd;
    rt->CreateSolidColorBrush(gsBg, &brGsBg);
    rt->CreateSolidColorBrush(gsBrd, &brGsBrd);
    const D2D1_ROUNDED_RECT gsRr = D2D1::RoundedRect(gsRc, 8.0f, 8.0f);
    if (brGsBg) rt->FillRoundedRectangle(gsRr, brGsBg.Get());
    if (brGsBrd) rt->DrawRoundedRectangle(gsRr, brGsBrd.Get(), 1.0f);

    if (brTextDark) {
        m_icons.Draw(rt, VectorIconId::Gamepad, D2D1::RectF(gsRc.left + 12.0f, gsRc.top + 17.0f, gsRc.left + 30.0f, gsRc.top + 35.0f), brTextDark.Get(), 1.8f);
    }

    std::wstring gsTitle = L"Game Shield";
    std::wstring gsSub = L"Select game to skip purge";
    std::wstring gsBadge = L"Select";
    bool badgeActive = false;

    if (m_lastGameStatus.anyRunning) {
        gsTitle = m_lastGameStatus.primaryGameName + L" Protected";
        gsSub = MemoryEngine::FormatBytes(m_lastGameStatus.combinedWorkingSetBytes) +
            (m_config.maximizeGameRam ? L" \u2022 Max RAM & Excluded" : L" \u2022 Purge Excluded");
        gsBadge = L"ACTIVE";
        badgeActive = true;
    } else if (!m_config.protectedGames.empty()) {
        wchar_t tbuf[64]{};
        _snwprintf_s(tbuf, _TRUNCATE, L"Game Shield (%zu)", m_config.protectedGames.size());
        gsTitle = tbuf;
        gsSub = L"Standby: " + m_config.protectedGames.front() + (m_config.protectedGames.size() > 1 ? L" + more" : L"");
        gsBadge = L"Armed";
    }

    const float badgeW = badgeActive ? 54.0f : 50.0f;
    const float badgeRight = gsRc.right - 29.0f;
    const float badgeLeft = badgeRight - badgeW;
    const D2D1_ROUNDED_RECT badgeRr = D2D1::RoundedRect(
        D2D1::RectF(badgeLeft, gsRc.top + 15.0f, badgeRight, gsRc.top + 37.0f), 5.0f, 5.0f);

    if (m_fmtBodySemi && brTextDark) {
        rt->DrawTextW(gsTitle.c_str(), static_cast<UINT32>(gsTitle.size()), m_fmtBodySemi.Get(),
                      D2D1::RectF(gsRc.left + 40.0f, gsRc.top + 8.0f, badgeLeft - 8.0f, gsRc.top + 26.0f),
                      brTextDark.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (m_fmtSub && brTextMuted) {
        rt->DrawTextW(gsSub.c_str(), static_cast<UINT32>(gsSub.size()), m_fmtSub.Get(),
                      D2D1::RectF(gsRc.left + 40.0f, gsRc.top + 26.0f, badgeLeft - 8.0f, gsRc.top + 43.0f),
                      brTextMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    rt->FillRoundedRectangle(badgeRr, badgeActive ? brPurple.Get() : brLavender.Get());
    if (m_fmtSmallBoldCenter) {
        rt->DrawTextW(gsBadge.c_str(), static_cast<UINT32>(gsBadge.size()), m_fmtSmallBoldCenter.Get(),
                      badgeRr.rect, badgeActive ? brWhite.Get() : brPurple.Get());
    }
    if (brTextMuted) {
        m_icons.Draw(rt, VectorIconId::ChevronRight,
                     D2D1::RectF(gsRc.right - 21.0f, gsRc.top + 20.5f, gsRc.right - 10.0f, gsRc.top + 31.5f),
                     brTextMuted.Get(), 2.2f);
    }

    // Row 4: Primary Purge CTA + Feedback status
    const bool busy = m_purgeRunning.load();
    DrawPrimaryButton(rt, D2D1::RectF(20.0f, 419.0f, w - 20.0f, 459.0f),
                      ControlId::PurgeBtn, busy ? L"Purging..." : L"Purge Memory",
                      13.0f, 8.0f, !busy);

    if (m_fmtSmallCenter) {
        rt->DrawTextW(m_purgeStatusMsg.c_str(), static_cast<UINT32>(m_purgeStatusMsg.size()), m_fmtSmallCenter.Get(),
                      D2D1::RectF(20.0f, 464.0f, w - 20.0f, 482.0f),
                      m_purgeStatusAccent ? brPurple.Get() : brTextMuted.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // Row 5: Footer Settings gear + Hide-to-Tray button
    DrawIconButton(rt, D2D1::RectF(20.0f, h - 40.0f, 52.0f, h - 8.0f),
                   ControlId::SettingsBtn, VectorIconId::Settings, 17.0f,
                   D2D1::ColorF(0x27272A, opacity), D2D1::ColorF(0xF1F1F4, opacity), D2D1::ColorF(0xE4E4E9, opacity),
                   1.8f, m_gearRotDeg, L"Settings");

    DrawIconButton(rt, D2D1::RectF(w - 52.0f, h - 40.0f, w - 20.0f, h - 8.0f),
                   ControlId::MinTrayBtn, VectorIconId::HideTray, 16.0f,
                   D2D1::ColorF(0x3F3F46, opacity), D2D1::ColorF(0xF1F1F4, opacity), D2D1::ColorF(0xE4E4E9, opacity),
                   1.8f, 0.0f, L"Hide to Tray");

    rt->SetTransform(oldTx);
}

void AppWindow::DrawGameView(ID2D1RenderTarget* rt, float w, float h, float slideX, float opacity) {
    D2D1_MATRIX_3X2_F oldTx{};
    rt->GetTransform(&oldTx);
    if (slideX != 0.0f) {
        rt->SetTransform(D2D1::Matrix3x2F::Translation(slideX, 0.0f) * oldTx);
    }

    ComPtr<ID2D1SolidColorBrush> brBlack, brDark, brMuted, brDivider, brPurple, brLavender, brWhite, brRed, brCardBorder;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x111113, opacity), &brBlack);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x18181B, opacity), &brDark);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x71717A, opacity), &brMuted);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xEFEFF4, opacity), &brDivider);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x6D28D9, opacity), &brPurple);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xF3EEFF, opacity), &brLavender);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, opacity), &brWhite);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xB91C1C, opacity), &brRed);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xE4E4E7, opacity), &brCardBorder);

    // Sub-Header
    DrawIconButton(rt, D2D1::RectF(20.0f, 56.0f, 48.0f, 84.0f),
                   ControlId::GameBackBtn, VectorIconId::Back, 15.0f,
                   D2D1::ColorF(0x18181B, opacity), D2D1::ColorF(0xF1F1F4, opacity), D2D1::ColorF(0xE4E4E9, opacity),
                   2.0f, 0.0f, L"Back");

    if (m_fmtHeaderTitle && brBlack) {
        rt->DrawTextW(L"Game Shield", 11, m_fmtHeaderTitle.Get(),
                      D2D1::RectF(56.0f, 56.0f, 240.0f, 84.0f), brBlack.Get());
    }

    DrawPrimaryButton(rt, D2D1::RectF(w - 102.0f, 57.0f, w - 20.0f, 84.0f),
                      ControlId::BoostGamesBtn, L"Boost RAM", 11.0f, 6.0f, true);

    if (brDivider) {
        rt->DrawLine(D2D1::Point2F(20.0f, 92.5f), D2D1::Point2F(w - 20.0f, 92.5f), brDivider.Get(), 1.0f);
    }

    // Scrollable region
    const float viewTop = 98.0f;
    const float viewBot = h - 12.0f;
    const float viewH = std::max(60.0f, viewBot - viewTop);
    const float contentX = 20.0f;
    const float contentW = w - 46.0f;
    const size_t vIdx = static_cast<size_t>(ActiveView::Games);
    m_scrollViewHeight = viewH;

    const D2D1_RECT_F clipRect = D2D1::RectF(contentX - 2.0f, viewTop, contentX + contentW + 2.0f, viewBot);
    rt->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_ALIASED);

    const float scrollY = m_scrollOffset[vIdx];
    float y = viewTop - scrollY;
    const float docStartY = y;

    // Helper for registering hits clipped to scroll viewport
    auto regScrolledHit = [&](ControlId id, const D2D1_RECT_F& rc, int arg = -1) {
        D2D1_RECT_F cl{
            rc.left,
            std::max(rc.top, viewTop),
            rc.right,
            std::min(rc.bottom, viewBot)
        };
        if (cl.bottom > cl.top && cl.right > cl.left) {
            RegisterHit(id, cl, nullptr, arg);
        }
    };

    // Maximize Game RAM Toggle Row
    DrawToggleRow(rt, contentX, y, contentW, 44.0f, VectorIconId::Count, false,
                  L"Maximize game RAM", L"Bypass purge, boost priority & EcoQoS off",
                  ToggleId::MaximizeGame, ControlId::ToggleMaximizeGame, m_config.maximizeGameRam);
    // Clip the toggle hit registered inside DrawToggleRow cleanly to scroll viewport
    if (!m_hitControls.empty() && m_hitControls.back().id == ControlId::ToggleMaximizeGame) {
        auto last = m_hitControls.back();
        m_hitControls.pop_back();
        regScrolledHit(last.id, last.rect);
    }
    y += 46.0f;

    ComPtr<ID2D1SolidColorBrush> brThinLine;
    rt->CreateSolidColorBrush(D2D1::ColorF(0xF1F1F5, opacity), &brThinLine);
    if (brThinLine) {
        rt->DrawLine(D2D1::Point2F(contentX, y), D2D1::Point2F(contentX + contentW, y), brThinLine.Get(), 1.0f);
    }
    y += 10.0f;

    // 2-Column Action Buttons: Browse .exe | Refresh Apps
    const float halfW = (contentW - 8.0f) * 0.5f;
    struct SmallCardBtn { ControlId id; float x; float w; VectorIconId icon; uint32_t iconColor; const wchar_t* label; };
    const SmallCardBtn actBtns[2] = {
        { ControlId::BrowseGameBtn,   contentX,             halfW, VectorIconId::FolderPlus, 0x6D28D9, L"Browse .exe" },
        { ControlId::RefreshProcsBtn, contentX + halfW + 8, halfW, VectorIconId::Refresh,    0x3F3F46, L"Refresh Apps" }
    };
    for (const auto& ab : actBtns) {
        D2D1_RECT_F brc = D2D1::RectF(ab.x, y, ab.x + ab.w, y + 34.0f);
        regScrolledHit(ab.id, brc);
        const bool hov = (m_hoverControl.id == ab.id);
        const bool prs = (m_pressedControl.id == ab.id);
        ComPtr<ID2D1SolidColorBrush> bgB, brdB, icB;
        rt->CreateSolidColorBrush(prs ? D2D1::ColorF(0xEDE9FE) : (hov ? D2D1::ColorF(0xF5F3FF) : D2D1::ColorF(0xFAFAFC)), &bgB);
        rt->CreateSolidColorBrush(prs ? D2D1::ColorF(0xC4B5FD) : (hov ? D2D1::ColorF(0xDDD6FE) : D2D1::ColorF(0xEAEAF2)), &brdB);
        rt->CreateSolidColorBrush(D2D1::ColorF(ab.iconColor), &icB);
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(brc, 8.0f, 8.0f);
        if (bgB) rt->FillRoundedRectangle(rr, bgB.Get());
        if (brdB) rt->DrawRoundedRectangle(rr, brdB.Get(), 1.0f);
        const float midX = brc.left + ab.w * 0.5f;
        const float iconL = midX - 48.0f;
        if (icB) m_icons.Draw(rt, ab.icon, D2D1::RectF(iconL, y + 10.5f, iconL + 13.0f, y + 23.5f), icB.Get(), 1.9f);
        if (m_fmtBodySemi && brDark) {
            rt->DrawTextW(ab.label, static_cast<UINT32>(wcslen(ab.label)), m_fmtBodySemi.Get(),
                          D2D1::RectF(iconL + 18.0f, y, brc.right - 6.0f, y + 34.0f), brDark.Get());
        }
    }
    y += 42.0f;

    // Action status line
    if (m_fmtSub) {
        rt->DrawTextW(m_gameActionMsg.c_str(), static_cast<UINT32>(m_gameActionMsg.size()), m_fmtSub.Get(),
                      D2D1::RectF(contentX, y, contentX + contentW, y + 16.0f),
                      m_gameActionAccent ? brPurple.Get() : brMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    y += 24.0f;

    // SELECTED GAMES section
    if (m_fmtSection && brMuted) {
        rt->DrawTextW(L"SELECTED GAMES", 14, m_fmtSection.Get(), D2D1::RectF(contentX, y, contentX + 160.0f, y + 15.0f), brMuted.Get());
    }
    if (m_fmtSub && brPurple) {
        ComPtr<IDWriteTextFormat> fmtRight;
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &fmtRight);
        if (fmtRight) {
            fmtRight->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            fmtRight->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawTextW(L"Protected & Maximized", 21, fmtRight.Get(),
                          D2D1::RectF(contentX + 140.0f, y, contentX + contentW, y + 15.0f), brPurple.Get());
        }
    }
    y += 20.0f;

    auto findRunningInfo = [&](const std::wstring& exe) -> const RunningProcessInfo* {
        for (const auto& rp : m_cachedRunningProcs) {
            if (_wcsicmp(rp.exeName.c_str(), exe.c_str()) == 0) return &rp;
        }
        return nullptr;
    };

    if (m_config.protectedGames.empty()) {
        const D2D1_ROUNDED_RECT emptyRr = D2D1::RoundedRect(D2D1::RectF(contentX, y, contentX + contentW, y + 38.0f), 7.0f, 7.0f);
        ComPtr<ID2D1SolidColorBrush> ebBg, ebBrd;
        rt->CreateSolidColorBrush(D2D1::ColorF(0xFAFAFC), &ebBg);
        rt->CreateSolidColorBrush(D2D1::ColorF(0xEFEFF4), &ebBrd);
        if (ebBg) rt->FillRoundedRectangle(emptyRr, ebBg.Get());
        if (ebBrd) rt->DrawRoundedRectangle(emptyRr, ebBrd.Get(), 1.0f);
        if (m_fmtSub && brMuted) {
            rt->DrawTextW(L"No games selected yet. Browse an .exe or click + below.", 55, m_fmtSub.Get(),
                          D2D1::RectF(contentX + 10.0f, y, contentX + contentW - 10.0f, y + 38.0f),
                          brMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        y += 46.0f;
    } else {
        for (size_t i = 0; i < m_config.protectedGames.size(); ++i) {
            const std::wstring& exe = m_config.protectedGames[i];
            const RunningProcessInfo* live = findRunningInfo(exe);
            const D2D1_RECT_F cardRc = D2D1::RectF(contentX, y, contentX + contentW, y + 46.0f);
            const D2D1_ROUNDED_RECT cardRr = D2D1::RoundedRect(cardRc, 7.0f, 7.0f);

            ComPtr<ID2D1SolidColorBrush> cBg, cBrd;
            rt->CreateSolidColorBrush(live ? D2D1::ColorF(0xF5F3FF) : D2D1::ColorF(0xFAFAFC), &cBg);
            rt->CreateSolidColorBrush(live ? D2D1::ColorF(0xD8B4FE) : D2D1::ColorF(0xE4E4E7), &cBrd);
            if (cBg) rt->FillRoundedRectangle(cardRr, cBg.Get());
            if (cBrd) rt->DrawRoundedRectangle(cardRr, cBrd.Get(), 1.0f);

            const D2D1_RECT_F remRc = D2D1::RectF(cardRc.right - 66.0f, y + 11.0f, cardRc.right - 6.0f, y + 35.0f);
            regScrolledHit(ControlId::RemoveProtectedGame, remRc, static_cast<int>(i));
            const bool rHov = (m_hoverControl.id == ControlId::RemoveProtectedGame && m_hoverControl.arg == static_cast<int>(i));
            const bool rPrs = (m_pressedControl.id == ControlId::RemoveProtectedGame && m_pressedControl.arg == static_cast<int>(i));
            ComPtr<ID2D1SolidColorBrush> remBg;
            rt->CreateSolidColorBrush(rPrs ? D2D1::ColorF(0xFECACA) : (rHov ? D2D1::ColorF(0xFEF2F2) : D2D1::ColorF(0xFFFFFF)), &remBg);
            const D2D1_ROUNDED_RECT remRr = D2D1::RoundedRect(remRc, 5.0f, 5.0f);
            if (remBg) rt->FillRoundedRectangle(remRr, remBg.Get());
            if (brCardBorder) rt->DrawRoundedRectangle(remRr, brCardBorder.Get(), 1.0f);
            if (m_fmtSmallBoldCenter && brRed) {
                rt->DrawTextW(L"Remove", 6, m_fmtSmallBoldCenter.Get(), remRc, brRed.Get());
            }

            if (m_fmtBodySemi && brDark) {
                rt->DrawTextW(exe.c_str(), static_cast<UINT32>(exe.size()), m_fmtBodySemi.Get(),
                              D2D1::RectF(cardRc.left + 10.0f, y + 6.0f, remRc.left - 8.0f, y + 23.0f),
                              brDark.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            const std::wstring sub = live
                ? (L"Running \u2022 " + MemoryEngine::FormatBytes(live->totalWorkingSetBytes) +
                   (m_config.maximizeGameRam ? L" (Maximized)" : L" (Protected)"))
                : L"Not running \u2022 Protected when launched";
            if (m_fmtSub) {
                rt->DrawTextW(sub.c_str(), static_cast<UINT32>(sub.size()), m_fmtSub.Get(),
                              D2D1::RectF(cardRc.left + 10.0f, y + 23.0f, remRc.left - 8.0f, y + 40.0f),
                              live ? brPurple.Get() : brMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            y += 51.0f;
        }
    }

    if (brThinLine) {
        rt->DrawLine(D2D1::Point2F(contentX, y + 3.0f), D2D1::Point2F(contentX + contentW, y + 3.0f), brThinLine.Get(), 1.0f);
    }
    y += 12.0f;

    // RUNNING APPS & GAMES section
    if (m_fmtSection && brMuted) {
        rt->DrawTextW(L"RUNNING APPS & GAMES", 20, m_fmtSection.Get(),
                      D2D1::RectF(contentX, y, contentX + 180.0f, y + 15.0f), brMuted.Get());
    }
    if (m_fmtSub && brMuted) {
        ComPtr<IDWriteTextFormat> fmtRight;
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &fmtRight);
        if (fmtRight) {
            fmtRight->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            fmtRight->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            rt->DrawTextW(L"Click + to protect", 18, fmtRight.Get(),
                          D2D1::RectF(contentX + 180.0f, y, contentX + contentW, y + 15.0f), brMuted.Get());
        }
    }
    y += 20.0f;

    m_shownRunningProcs.clear();
    int shownCount = 0;
    for (const auto& proc : m_cachedRunningProcs) {
        bool alreadyProtected = false;
        for (const auto& pg : m_config.protectedGames) {
            if (_wcsicmp(pg.c_str(), proc.exeName.c_str()) == 0) {
                alreadyProtected = true;
                break;
            }
        }
        if (alreadyProtected) continue;
        if (shownCount >= 18) break;
        const int argIdx = shownCount++;
        m_shownRunningProcs.push_back(proc.exeName);

        const D2D1_RECT_F cardRc = D2D1::RectF(contentX, y, contentX + contentW, y + 42.0f);
        const D2D1_ROUNDED_RECT cardRr = D2D1::RoundedRect(cardRc, 7.0f, 7.0f);
        if (brWhite) rt->FillRoundedRectangle(cardRr, brWhite.Get());
        if (brDivider) rt->DrawRoundedRectangle(cardRr, brDivider.Get(), 1.0f);

        const D2D1_RECT_F addRc = D2D1::RectF(cardRc.right - 72.0f, y + 9.0f, cardRc.right - 6.0f, y + 33.0f);
        regScrolledHit(ControlId::AddRunningProcess, addRc, argIdx);
        const bool aHov = (m_hoverControl.id == ControlId::AddRunningProcess && m_hoverControl.arg == argIdx);
        const bool aPrs = (m_pressedControl.id == ControlId::AddRunningProcess && m_pressedControl.arg == argIdx);
        ComPtr<ID2D1SolidColorBrush> addBg;
        rt->CreateSolidColorBrush(aPrs ? D2D1::ColorF(0xDDD6FE) : (aHov ? D2D1::ColorF(0xEDE9FE) : D2D1::ColorF(0xF3EEFF)), &addBg);
        const D2D1_ROUNDED_RECT addRr = D2D1::RoundedRect(addRc, 5.0f, 5.0f);
        if (addBg) rt->FillRoundedRectangle(addRr, addBg.Get());
        if (m_fmtSmallBoldCenter && brPurple) {
            rt->DrawTextW(L"+ Protect", 9, m_fmtSmallBoldCenter.Get(), addRc, brPurple.Get());
        }

        if (m_fmtBodySemi && brDark) {
            rt->DrawTextW(proc.exeName.c_str(), static_cast<UINT32>(proc.exeName.size()), m_fmtBodySemi.Get(),
                          D2D1::RectF(cardRc.left + 10.0f, y + 5.0f, addRc.left - 8.0f, y + 21.0f),
                          brDark.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        wchar_t rbuf[64]{};
        _snwprintf_s(rbuf, _TRUNCATE, L"%s%s",
                     MemoryEngine::FormatBytes(proc.totalWorkingSetBytes).c_str(),
                     proc.hasWindow ? L" \u2022 App Window" : L" \u2022 Background");
        if (m_fmtSub && brMuted) {
            rt->DrawTextW(rbuf, static_cast<UINT32>(wcslen(rbuf)), m_fmtSub.Get(),
                          D2D1::RectF(cardRc.left + 10.0f, y + 21.0f, addRc.left - 8.0f, y + 37.0f),
                          brMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        y += 47.0f;
    }

    rt->PopAxisAlignedClip();

    m_scrollContentHeight[vIdx] = std::max(0.0f, y - docStartY + 8.0f);
    m_scrollMaxOffset[vIdx] = std::max(0.0f, m_scrollContentHeight[vIdx] - viewH);
    m_scrollOffset[vIdx] = std::clamp(m_scrollOffset[vIdx], 0.0f, m_scrollMaxOffset[vIdx]);

    DrawCustomScrollbar(rt, w - 18.0f, viewTop + 2.0f, viewH - 4.0f,
                        m_scrollContentHeight[vIdx], m_scrollOffset[vIdx]);

    rt->SetTransform(oldTx);
}

void AppWindow::DrawSettingsView(ID2D1RenderTarget* rt, float w, float h, float slideX, float opacity) {
    D2D1_MATRIX_3X2_F oldTx{};
    rt->GetTransform(&oldTx);
    if (slideX != 0.0f) {
        rt->SetTransform(D2D1::Matrix3x2F::Translation(slideX, 0.0f) * oldTx);
    }

    ComPtr<ID2D1SolidColorBrush> brBlack, brDark, brMed, brMuted, brDivider, brLine, brPurple, brCardBorder, brWhite;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x111113, opacity), &brBlack);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x18181B, opacity), &brDark);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x52525B, opacity), &brMed);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x71717A, opacity), &brMuted);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xEFEFF4, opacity), &brDivider);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xF4F4F6, opacity), &brLine);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x6D28D9, opacity), &brPurple);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xE4E4E7, opacity), &brCardBorder);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, opacity), &brWhite);

    DrawIconButton(rt, D2D1::RectF(20.0f, 56.0f, 48.0f, 84.0f),
                   ControlId::BackBtn, VectorIconId::Back, 15.0f,
                   D2D1::ColorF(0x18181B, opacity), D2D1::ColorF(0xF1F1F4, opacity), D2D1::ColorF(0xE4E4E9, opacity),
                   2.0f, 0.0f, L"Back");

    if (m_fmtHeaderTitle && brBlack) {
        rt->DrawTextW(L"Settings", 8, m_fmtHeaderTitle.Get(),
                      D2D1::RectF(56.0f, 56.0f, 240.0f, 84.0f), brBlack.Get());
    }
    if (brDivider) {
        rt->DrawLine(D2D1::Point2F(20.0f, 92.5f), D2D1::Point2F(w - 20.0f, 92.5f), brDivider.Get(), 1.0f);
    }

    const float viewTop = 98.0f;
    const float viewBot = h - 12.0f;
    const float viewH = std::max(60.0f, viewBot - viewTop);
    const float contentX = 20.0f;
    const float contentW = w - 46.0f;
    const size_t vIdx = static_cast<size_t>(ActiveView::Settings);
    m_scrollViewHeight = viewH;

    const D2D1_RECT_F clipRect = D2D1::RectF(contentX - 2.0f, viewTop, contentX + contentW + 2.0f, viewBot);
    rt->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_ALIASED);

    const float scrollY = m_scrollOffset[vIdx];
    float y = viewTop - scrollY;
    const float docStartY = y;

    auto regScrolledHit = [&](ControlId id, const D2D1_RECT_F& rc, const wchar_t* tip = nullptr) {
        D2D1_RECT_F cl{
            rc.left,
            std::max(rc.top, viewTop),
            rc.right,
            std::min(rc.bottom, viewBot)
        };
        if (cl.bottom > cl.top && cl.right > cl.left) {
            RegisterHit(id, cl, tip);
        }
    };

    auto clipLastRegistered = [&]() {
        if (m_hitControls.empty()) return;
        HitControl last = m_hitControls.back();
        m_hitControls.pop_back();
        regScrolledHit(last.id, last.rect, last.tooltip.empty() ? nullptr : last.tooltip.c_str());
    };

    auto drawSection = [&](const wchar_t* title) {
        if (m_fmtSection && brMuted) {
            rt->DrawTextW(title, static_cast<UINT32>(wcslen(title)), m_fmtSection.Get(),
                          D2D1::RectF(contentX, y + 2.0f, contentX + contentW, y + 18.0f), brMuted.Get());
        }
        y += 20.0f;
    };

    auto drawDivider = [&]() {
        if (brLine) {
            rt->DrawLine(D2D1::Point2F(contentX, y), D2D1::Point2F(contentX + contentW, y), brLine.Get(), 1.0f);
        }
        y += 1.0f;
    };

    auto drawTgl = [&](VectorIconId icon, const wchar_t* label, ToggleId tId, ControlId cId, bool val, bool lastInGroup = false) {
        DrawToggleRow(rt, contentX, y, contentW, 38.0f, icon, true, label, nullptr, tId, cId, val);
        clipLastRegistered();
        y += 38.0f;
        if (!lastInGroup) drawDivider();
    };

    // STARTUP & TRAY
    drawSection(L"STARTUP & TRAY");
    drawTgl(VectorIconId::Power,          L"Run at startup",   ToggleId::AutoStart,      ControlId::ToggleAutoStart,      m_config.autoStart);
    drawTgl(VectorIconId::StartMinimized, L"Start minimized",  ToggleId::StartMinimized, ControlId::ToggleStartMinimized, m_config.startMinimized);
    drawTgl(VectorIconId::MinimizeToTray, L"Minimize to tray", ToggleId::MinimizeToTray, ControlId::ToggleMinToTray,      m_config.minimizeToTray);
    drawTgl(VectorIconId::CloseToTray,    L"Close to tray",    ToggleId::CloseToTray,    ControlId::ToggleCloseToTray,    m_config.closeToTray);

    // Tray icon segmented pill row
    {
        if (brDark) m_icons.Draw(rt, VectorIconId::Lightning, D2D1::RectF(contentX, y + 12.5f, contentX + 15.0f, y + 27.5f), brDark.Get(), 1.8f);
        if (m_fmtRowTitle && brDark) {
            rt->DrawTextW(L"Tray icon", 9, m_fmtRowTitle.Get(), D2D1::RectF(contentX + 25.0f, y, contentX + 190.0f, y + 40.0f), brDark.Get());
        }
        const bool isPct = (m_config.trayStyle == L"percent");
        const float pillW = 116.0f;
        const D2D1_ROUNDED_RECT pillBox = D2D1::RoundedRect(
            D2D1::RectF(contentX + contentW - pillW, y + 5.0f, contentX + contentW, y + 35.0f), 7.0f, 7.0f);
        if (brLine) rt->FillRoundedRectangle(pillBox, brLine.Get());
        if (brCardBorder) rt->DrawRoundedRectangle(pillBox, brCardBorder.Get(), 1.0f);

        const D2D1_RECT_F logoRc = D2D1::RectF(pillBox.rect.left + 3.0f, y + 8.0f, pillBox.rect.left + 52.0f, y + 32.0f);
        const D2D1_RECT_F pctRc  = D2D1::RectF(pillBox.rect.left + 54.0f, y + 8.0f, pillBox.rect.right - 3.0f, y + 32.0f);
        regScrolledHit(ControlId::TrayLogoBtn, logoRc);
        regScrolledHit(ControlId::TrayPercentBtn, pctRc);

        if (!isPct && brPurple) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(logoRc, 5.0f, 5.0f), brPurple.Get());
        } else if (isPct && brPurple) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(pctRc, 5.0f, 5.0f), brPurple.Get());
        }
        if (m_fmtSmallBoldCenter) {
            rt->DrawTextW(L"Icon", 4, m_fmtSmallBoldCenter.Get(), logoRc, !isPct ? brWhite.Get() : brMed.Get());
            rt->DrawTextW(L"RAM %", 5, m_fmtSmallBoldCenter.Get(), pctRc, isPct ? brWhite.Get() : brMed.Get());
        }
        y += 40.0f;
        drawDivider();
    }

    drawTgl(VectorIconId::Pin, L"Always on top", ToggleId::AlwaysOnTop, ControlId::ToggleAlwaysOnTop, m_config.alwaysOnTop, true);
    y += 10.0f;

    // PURGE TARGETS
    drawSection(L"PURGE TARGETS");
    drawTgl(VectorIconId::WorkingSets,  L"Working sets",  ToggleId::WorkingSets,  ControlId::ToggleWorkingSets,  m_config.purgeWorkingSets);
    drawTgl(VectorIconId::SystemCache,  L"System cache",  ToggleId::SystemCache,  ControlId::ToggleSystemCache,  m_config.purgeSystemCache);
    drawTgl(VectorIconId::StandbyList,  L"Standby list",  ToggleId::StandbyList,  ControlId::ToggleStandbyList,  m_config.purgeStandbyList);
    drawTgl(VectorIconId::ModifiedList, L"Modified list", ToggleId::ModifiedList, ControlId::ToggleModifiedList, m_config.purgeModifiedList, true);
    y += 10.0f;

    // AUTOMATION
    drawSection(L"AUTOMATION");
    drawTgl(VectorIconId::Gauge, L"Auto-purge", ToggleId::AutoPurge, ControlId::ToggleAutoPurge, m_config.autoPurgeEnabled);

    auto drawStepper = [&](VectorIconId icon, const wchar_t* label, const std::wstring& valStr,
                           ControlId prevId, ControlId nextId) {
        if (brMuted) m_icons.Draw(rt, icon, D2D1::RectF(contentX, y + 12.5f, contentX + 15.0f, y + 27.5f), brMuted.Get(), 1.8f);
        if (m_fmtRowTitle && brDark) {
            rt->DrawTextW(label, static_cast<UINT32>(wcslen(label)), m_fmtRowTitle.Get(),
                          D2D1::RectF(contentX + 25.0f, y, contentX + 190.0f, y + 40.0f), brDark.Get());
        }

        const float stepW = 94.0f;
        const D2D1_ROUNDED_RECT stepBox = D2D1::RoundedRect(
            D2D1::RectF(contentX + contentW - stepW, y + 5.0f, contentX + contentW, y + 35.0f), 7.0f, 7.0f);
        if (brLine) rt->FillRoundedRectangle(stepBox, brLine.Get());
        if (brCardBorder) rt->DrawRoundedRectangle(stepBox, brCardBorder.Get(), 1.0f);

        const D2D1_RECT_F prevRc = D2D1::RectF(stepBox.rect.left + 2.0f, y + 8.0f, stepBox.rect.left + 26.0f, y + 32.0f);
        const D2D1_RECT_F nextRc = D2D1::RectF(stepBox.rect.right - 26.0f, y + 8.0f, stepBox.rect.right - 2.0f, y + 32.0f);
        DrawIconButton(rt, prevRc, prevId, VectorIconId::ChevronLeft, 11.0f,
                       D2D1::ColorF(0x3F3F46), D2D1::ColorF(0xE4E4E9), D2D1::ColorF(0xD4D4D8), 2.2f);
        clipLastRegistered();
        DrawIconButton(rt, nextRc, nextId, VectorIconId::ChevronRight, 11.0f,
                       D2D1::ColorF(0x3F3F46), D2D1::ColorF(0xE4E4E9), D2D1::ColorF(0xD4D4D8), 2.2f);
        clipLastRegistered();

        if (m_fmtSmallBoldCenter && brPurple) {
            rt->DrawTextW(valStr.c_str(), static_cast<UINT32>(valStr.size()), m_fmtSmallBoldCenter.Get(),
                          D2D1::RectF(prevRc.right, y + 5.0f, nextRc.left, y + 35.0f), brPurple.Get());
        }
        y += 40.0f;
        drawDivider();
    };

    wchar_t thrBuf[16]{};
    _snwprintf_s(thrBuf, _TRUNCATE, L"%d%%", m_config.autoPurgeThreshold);
    drawStepper(VectorIconId::Gauge, L"Threshold", thrBuf, ControlId::ThresholdPrevBtn, ControlId::ThresholdNextBtn);

    std::wstring intStr = L"Off";
    if (m_config.autoPurgeIntervalMinutes > 0) {
        wchar_t ibuf[16]{};
        _snwprintf_s(ibuf, _TRUNCATE, L"%dm", m_config.autoPurgeIntervalMinutes);
        intStr = ibuf;
    }
    drawStepper(VectorIconId::Clock, L"Interval", intStr, ControlId::IntervalPrevBtn, ControlId::IntervalNextBtn);

    drawTgl(VectorIconId::Bell, L"Notifications", ToggleId::Notifications, ControlId::ToggleNotifications, m_config.showNotifications, true);

    // Elevation Row when not elevated
    if (!m_isElevated) {
        y += 6.0f;
        if (brDivider) rt->DrawLine(D2D1::Point2F(contentX, y), D2D1::Point2F(contentX + contentW, y), brDivider.Get(), 1.0f);
        y += 4.0f;
        if (brDark) m_icons.Draw(rt, VectorIconId::Shield, D2D1::RectF(contentX, y + 11.5f, contentX + 15.0f, y + 26.5f), brDark.Get(), 1.8f);
        if (m_fmtRowTitle && brDark) {
            rt->DrawTextW(L"Administrator mode", 18, m_fmtRowTitle.Get(), D2D1::RectF(contentX + 25.0f, y, contentX + contentW - 76.0f, y + 38.0f), brDark.Get());
        }
        const D2D1_RECT_F elvRc = D2D1::RectF(contentX + contentW - 66.0f, y + 5.0f, contentX + contentW, y + 33.0f);
        regScrolledHit(ControlId::ElevateBtn, elvRc);
        const bool eHov = (m_hoverControl.id == ControlId::ElevateBtn);
        const bool ePrs = (m_pressedControl.id == ControlId::ElevateBtn);
        ComPtr<ID2D1SolidColorBrush> elvBg;
        rt->CreateSolidColorBrush(ePrs ? D2D1::ColorF(0xE4E4E9) : (eHov ? D2D1::ColorF(0xF1F1F4) : D2D1::ColorF(0xFFFFFF)), &elvBg);
        const D2D1_ROUNDED_RECT elvRr = D2D1::RoundedRect(elvRc, 7.0f, 7.0f);
        if (elvBg) rt->FillRoundedRectangle(elvRr, elvBg.Get());
        if (brCardBorder) rt->DrawRoundedRectangle(elvRr, brCardBorder.Get(), 1.0f);
        if (m_fmtSmallBoldCenter && brPurple) {
            rt->DrawTextW(L"Elevate", 7, m_fmtSmallBoldCenter.Get(), elvRc, brPurple.Get());
        }
        y += 38.0f;
    }

    // Footer: VRam v2.1 / Created by : @vinnotdkk / Support Developer
    y += 8.0f;
    if (brDivider) rt->DrawLine(D2D1::Point2F(contentX, y), D2D1::Point2F(contentX + contentW, y), brDivider.Get(), 1.0f);
    y += 12.0f;

    if (m_fmtRowTitle && brBlack) {
        ComPtr<IDWriteTextFormat> fmtVerTitle;
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &fmtVerTitle);
        if (fmtVerTitle) {
            rt->DrawTextW(L"VRam", 4, fmtVerTitle.Get(), D2D1::RectF(contentX, y, contentX + 42.0f, y + 18.0f), brBlack.Get());
        }
    }
    if (m_fmtBodySemi && brPurple) {
        rt->DrawTextW(L"v2.1", 4, m_fmtBodySemi.Get(), D2D1::RectF(contentX + 41.0f, y + 1.0f, contentX + 86.0f, y + 18.0f), brPurple.Get());
    }
    if (m_fmtBody && brMed) {
        rt->DrawTextW(L"Created by : @vinnotdkk", 23, m_fmtBody.Get(),
                      D2D1::RectF(contentX, y + 19.0f, contentX + 178.0f, y + 36.0f), brMed.Get());
    }

    const D2D1_RECT_F supRc = D2D1::RectF(contentX + contentW - 142.0f, y + 2.0f, contentX + contentW, y + 34.0f);
    DrawPrimaryButton(rt, supRc, ControlId::SupportDevBtn, L"Support Developer",
                      11.5f, 7.0f, true, VectorIconId::Heart, L"https://sociabuzz.com/idnvinnmalay");
    clipLastRegistered();
    y += 42.0f;

    rt->PopAxisAlignedClip();

    m_scrollContentHeight[vIdx] = std::max(0.0f, y - docStartY + 6.0f);
    m_scrollMaxOffset[vIdx] = std::max(0.0f, m_scrollContentHeight[vIdx] - viewH);
    m_scrollOffset[vIdx] = std::clamp(m_scrollOffset[vIdx], 0.0f, m_scrollMaxOffset[vIdx]);

    DrawCustomScrollbar(rt, w - 18.0f, viewTop + 2.0f, viewH - 4.0f,
                        m_scrollContentHeight[vIdx], m_scrollOffset[vIdx]);

    rt->SetTransform(oldTx);
}

void AppWindow::DrawToggleRow(ID2D1RenderTarget* rt,
                              float x, float y, float w, float h,
                              VectorIconId icon, bool hasIcon,
                              const wchar_t* title, const wchar_t* subtitle,
                              ToggleId toggleId, ControlId controlId, bool checked) {
    ComPtr<ID2D1SolidColorBrush> brDark, brMuted;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x18181B), &brDark);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x71717A), &brMuted);

    const float textLeft = hasIcon ? (x + 25.0f) : x;
    if (hasIcon && brDark) {
        const float iy = y + (h - 15.0f) * 0.5f;
        m_icons.Draw(rt, icon, D2D1::RectF(x, iy, x + 15.0f, iy + 15.0f), brDark.Get(), 1.8f);
    }

    const float switchLeft = x + w - 40.0f;
    const float switchTop  = y + (h - 22.0f) * 0.5f;
    const D2D1_RECT_F switchRc = D2D1::RectF(switchLeft, switchTop, switchLeft + 40.0f, switchTop + 22.0f);

    if (subtitle) {
        if (m_fmtBodySemi && brDark) {
            rt->DrawTextW(title, static_cast<UINT32>(wcslen(title)), m_fmtBodySemi.Get(),
                          D2D1::RectF(textLeft, y + 4.0f, switchLeft - 10.0f, y + 22.0f), brDark.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (m_fmtSub && brMuted) {
            rt->DrawTextW(subtitle, static_cast<UINT32>(wcslen(subtitle)), m_fmtSub.Get(),
                          D2D1::RectF(textLeft, y + 22.0f, switchLeft - 10.0f, y + 39.0f), brMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    } else if (m_fmtRowTitle && brDark) {
        rt->DrawTextW(title, static_cast<UINT32>(wcslen(title)), m_fmtRowTitle.Get(),
                      D2D1::RectF(textLeft, y, switchLeft - 10.0f, y + h), brDark.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    RegisterHit(controlId, D2D1::RectF(x, y, x + w, y + h));
    const bool hov = (m_hoverControl.id == controlId);
    const float animT = m_togglePos[static_cast<size_t>(toggleId)];
    DrawToggleSwitch(rt, switchRc, (animT >= 0.0f && animT <= 1.0f) ? animT : (checked ? 1.0f : 0.0f), hov);
}

void AppWindow::DrawToggleSwitch(ID2D1RenderTarget* rt, const D2D1_RECT_F& rect, float animT, bool hovered) {
    const D2D1_ROUNDED_RECT trackRr = D2D1::RoundedRect(rect, 11.0f, 11.0f);
    ComPtr<ID2D1SolidColorBrush> brTrack, brTrackStroke, brKnob;

    const D2D1_COLOR_F fillCol = LerpColor(hovered ? 0xDCDCE2u : 0xE4E4E7u, hovered ? 0x5B21B6u : 0x6D28D9u, animT);
    const D2D1_COLOR_F brdCol  = LerpColor(0xD4D4D8u, 0x6D28D9u, animT);
    const D2D1_COLOR_F knobCol = LerpColor(0x52525Bu, 0xFFFFFFu, animT);

    rt->CreateSolidColorBrush(fillCol, &brTrack);
    rt->CreateSolidColorBrush(brdCol, &brTrackStroke);
    rt->CreateSolidColorBrush(knobCol, &brKnob);

    if (brTrack) rt->FillRoundedRectangle(trackRr, brTrack.Get());
    if (brTrackStroke && animT < 0.99f) rt->DrawRoundedRectangle(trackRr, brTrackStroke.Get(), 1.0f);

    const float knobCx = rect.left + 11.0f + animT * 18.0f;
    const float knobCy = (rect.top + rect.bottom) * 0.5f;
    const float knobR  = 7.0f + animT * 1.0f + (hovered ? 0.5f : 0.0f);
    if (brKnob) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobCx, knobCy), knobR, knobR), brKnob.Get());
    }
}

void AppWindow::DrawIconButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc,
                               ControlId id, VectorIconId icon, float iconDip,
                               D2D1_COLOR_F fg, D2D1_COLOR_F hoverBg, D2D1_COLOR_F pressBg,
                               float strokeW, float rotDeg, const wchar_t* tooltip) {
    RegisterHit(id, rc, tooltip);
    const bool hov = (m_hoverControl.id == id);
    const bool prs = (m_pressedControl.id == id);

    if (hov || prs) {
        ComPtr<ID2D1SolidColorBrush> brBg;
        rt->CreateSolidColorBrush(prs ? pressBg : hoverBg, &brBg);
        if (brBg) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brBg.Get());
        }
    }

    const float cx = (rc.left + rc.right) * 0.5f;
    const float cy = (rc.top + rc.bottom) * 0.5f;
    const float scale = prs ? 0.90f : (hov ? 1.06f : 1.0f);
    const float half = iconDip * 0.5f * scale;

    ComPtr<ID2D1SolidColorBrush> brFg;
    rt->CreateSolidColorBrush(fg, &brFg);
    if (brFg) {
        m_icons.Draw(rt, icon, D2D1::RectF(cx - half, cy - half, cx + half, cy + half), brFg.Get(), strokeW, rotDeg);
    }
}

void AppWindow::DrawPrimaryButton(ID2D1RenderTarget* rt, const D2D1_RECT_F& rc,
                                  ControlId id, const wchar_t* text, float fontSize,
                                  float cornerRadius, bool enabled, VectorIconId prefixIcon,
                                  const wchar_t* tooltip) {
    if (enabled) {
        RegisterHit(id, rc, tooltip);
    }
    const bool hov = enabled && (m_hoverControl.id == id);
    const bool prs = enabled && (m_pressedControl.id == id);

    D2D1_RECT_F drawRc = rc;
    if (prs) {
        drawRc.left += 1.5f; drawRc.right -= 1.5f;
        drawRc.top += 0.6f; drawRc.bottom -= 0.6f;
    }

    const float opa = enabled ? 1.0f : 0.60f;
    const D2D1_COLOR_F bgCol = prs ? D2D1::ColorF(0x4C1D95, opa) : (hov ? D2D1::ColorF(0x5B21B6, opa) : D2D1::ColorF(0x6D28D9, opa));
    ComPtr<ID2D1SolidColorBrush> brBg, brBorder, brWhite;
    rt->CreateSolidColorBrush(bgCol, &brBg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0x5B21B6, opa), &brBorder);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, opa), &brWhite);

    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(drawRc, cornerRadius, cornerRadius);
    if (brBg) rt->FillRoundedRectangle(rr, brBg.Get());
    if (brBorder) rt->DrawRoundedRectangle(rr, brBorder.Get(), 1.0f);

    ComPtr<IDWriteTextFormat> fmt;
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", &fmt);
    if (fmt && brWhite) {
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (prefixIcon != VectorIconId::Count) {
            const float cy = (drawRc.top + drawRc.bottom) * 0.5f;
            m_icons.Draw(rt, prefixIcon,
                         D2D1::RectF(drawRc.left + 12.0f, cy - 6.5f, drawRc.left + 25.0f, cy + 6.5f),
                         brWhite.Get(), 1.5f);
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            rt->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmt.Get(),
                          D2D1::RectF(drawRc.left + 31.0f, drawRc.top, drawRc.right - 8.0f, drawRc.bottom), brWhite.Get());
        } else {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            rt->DrawTextW(text, static_cast<UINT32>(wcslen(text)), fmt.Get(), drawRc, brWhite.Get());
        }
    }
}

void AppWindow::DrawCustomScrollbar(ID2D1RenderTarget* rt, float x, float topY, float viewH,
                                    float contentH, float scrollOffset) {
    m_activeScrollTrack = D2D1::RectF(x - 4.0f, topY, x + 8.0f, topY + viewH);
    if (contentH <= viewH + 1.0f || viewH <= 0.0f) return;

    const float thumbH = std::max(28.0f, viewH * (viewH / contentH));
    const float maxOffset = contentH - viewH;
    const float ratio = (maxOffset > 0.0f) ? std::clamp(scrollOffset / maxOffset, 0.0f, 1.0f) : 0.0f;
    const float thumbY = topY + ratio * (viewH - thumbH);

    const D2D1_RECT_F thumbHitRc = D2D1::RectF(x - 4.0f, thumbY, x + 8.0f, thumbY + thumbH);
    RegisterHit(ControlId::ScrollThumb, thumbHitRc);

    const bool active = m_draggingScrollThumb || (m_hoverControl.id == ControlId::ScrollThumb);
    ComPtr<ID2D1SolidColorBrush> brThumb;
    rt->CreateSolidColorBrush(active ? D2D1::ColorF(0xA1A1AA, 0.95f) : D2D1::ColorF(0xD4D4D8, 0.85f), &brThumb);
    if (brThumb) {
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, thumbY, x + 4.0f, thumbY + thumbH), 2.0f, 2.0f), brThumb.Get());
    }
}

void AppWindow::DrawTooltipOverlay(ID2D1RenderTarget* rt, float w, float h) {
    if (!m_tooltipVisible || m_hoverTooltipText.empty() || !m_dwriteFactory) return;

    ComPtr<IDWriteTextFormat> tipFmt;
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"en-us", &tipFmt);
    if (!tipFmt) return;
    tipFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    tipFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(m_hoverTooltipText.c_str(), static_cast<UINT32>(m_hoverTooltipText.size()),
                                      tipFmt.Get(), 300.0f, 30.0f, &layout);
    DWRITE_TEXT_METRICS tm{};
    float tipW = 80.0f;
    if (layout && SUCCEEDED(layout->GetMetrics(&tm))) {
        tipW = std::min(300.0f, tm.width + 16.0f);
    }
    const float tipH = 22.0f;

    float tx = ((m_hoverTooltipAnchor.left + m_hoverTooltipAnchor.right) * 0.5f) - tipW * 0.5f;
    tx = std::clamp(tx, 8.0f, std::max(8.0f, w - tipW - 8.0f));
    float ty = m_hoverTooltipAnchor.top - tipH - 6.0f;
    if (ty < 8.0f) {
        ty = std::min(h - tipH - 8.0f, m_hoverTooltipAnchor.bottom + 6.0f);
    }

    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(tx, ty, tx + tipW, ty + tipH), 5.0f, 5.0f);
    ComPtr<ID2D1SolidColorBrush> brBg, brFg;
    rt->CreateSolidColorBrush(D2D1::ColorF(0x18181B, 0.94f), &brBg);
    rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 1.0f), &brFg);
    if (brBg) rt->FillRoundedRectangle(rr, brBg.Get());
    if (brFg) rt->DrawTextW(m_hoverTooltipText.c_str(), static_cast<UINT32>(m_hoverTooltipText.size()), tipFmt.Get(), rr.rect, brFg.Get());
}

void AppWindow::RegisterHit(ControlId id, const D2D1_RECT_F& rc, const wchar_t* tooltip, int arg) {
    HitControl hc;
    hc.id = id;
    hc.arg = arg;
    hc.rect = rc;
    if (tooltip) hc.tooltip = tooltip;
    m_hitControls.push_back(std::move(hc));
}

AppWindow::ControlTarget AppWindow::HitTest(float dipX, float dipY, std::wstring* outTooltip, D2D1_RECT_F* outRect) const {
    for (auto it = m_hitControls.rbegin(); it != m_hitControls.rend(); ++it) {
        if (dipX >= it->rect.left && dipX <= it->rect.right &&
            dipY >= it->rect.top && dipY <= it->rect.bottom) {
            if (outTooltip) *outTooltip = it->tooltip;
            if (outRect) *outRect = it->rect;
            return {it->id, it->arg};
        }
    }
    if (outTooltip) outTooltip->clear();
    return {};
}

void AppWindow::ActivateControl(const ControlTarget& target) {
    switch (target.id) {
    case ControlId::WinMinimize:
        if (m_config.minimizeToTray) HideToTray();
        else MinimizeWindow();
        break;
    case ControlId::WinClose:
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
        break;
    case ControlId::GameShieldCard:
        SwitchToView(ActiveView::Games);
        break;
    case ControlId::PurgeBtn:
        TriggerPurgeAsync(false);
        break;
    case ControlId::SettingsBtn:
        SwitchToView(ActiveView::Settings);
        break;
    case ControlId::MinTrayBtn:
        HideToTray();
        break;
    case ControlId::GameBackBtn:
    case ControlId::BackBtn:
        SwitchToView(ActiveView::Main);
        break;
    case ControlId::BoostGamesBtn:
        MemoryEngine::EnableAvailablePrivileges();
        RefreshGameProtectionUi(true, true);
        if (m_config.protectedGames.empty()) {
            m_gameActionMsg = L"Add at least one game .exe first to boost.";
            m_gameActionAccent = false;
        } else if (m_lastGameStatus.anyRunning) {
            wchar_t buf[160]{};
            _snwprintf_s(buf, _TRUNCATE, L"Boosted %s (%s kept resident in RAM)",
                         m_lastGameStatus.primaryGameName.c_str(),
                         MemoryEngine::FormatBytes(m_lastGameStatus.combinedWorkingSetBytes).c_str());
            m_gameActionMsg = buf;
            m_gameActionAccent = true;
        } else {
            m_gameActionMsg = L"Armed: game will auto-boost on launch.";
            m_gameActionAccent = false;
        }
        break;
    case ControlId::ToggleMaximizeGame:
        m_config.maximizeGameRam = !m_config.maximizeGameRam;
        SaveAndApplyConfig();
        break;
    case ControlId::BrowseGameBtn:
        BrowseForGameExe();
        break;
    case ControlId::RefreshProcsBtn:
        RefreshGameProtectionUi(false, true);
        break;
    case ControlId::RemoveProtectedGame:
        if (target.arg >= 0 && static_cast<size_t>(target.arg) < m_config.protectedGames.size()) {
            RemoveProtectedGameExe(m_config.protectedGames[static_cast<size_t>(target.arg)]);
        }
        break;
    case ControlId::AddRunningProcess:
        if (target.arg >= 0 && static_cast<size_t>(target.arg) < m_shownRunningProcs.size()) {
            AddProtectedGameExe(m_shownRunningProcs[static_cast<size_t>(target.arg)]);
        }
        break;
    case ControlId::ToggleAutoStart:
        m_config.autoStart = !m_config.autoStart;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleStartMinimized:
        m_config.startMinimized = !m_config.startMinimized;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleMinToTray:
        m_config.minimizeToTray = !m_config.minimizeToTray;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleCloseToTray:
        m_config.closeToTray = !m_config.closeToTray;
        SaveAndApplyConfig();
        break;
    case ControlId::TrayLogoBtn:
        if (m_config.trayStyle != L"logo") {
            m_config.trayStyle = L"logo";
            SaveAndApplyConfig();
        }
        break;
    case ControlId::TrayPercentBtn:
        if (m_config.trayStyle != L"percent") {
            m_config.trayStyle = L"percent";
            SaveAndApplyConfig();
        }
        break;
    case ControlId::ToggleAlwaysOnTop:
        m_config.alwaysOnTop = !m_config.alwaysOnTop;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleWorkingSets:
        m_config.purgeWorkingSets = !m_config.purgeWorkingSets;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleSystemCache:
        m_config.purgeSystemCache = !m_config.purgeSystemCache;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleStandbyList:
        m_config.purgeStandbyList = !m_config.purgeStandbyList;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleModifiedList:
        m_config.purgeModifiedList = !m_config.purgeModifiedList;
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleAutoPurge:
        m_config.autoPurgeEnabled = !m_config.autoPurgeEnabled;
        SaveAndApplyConfig();
        break;
    case ControlId::ThresholdPrevBtn:
        m_config.autoPurgeThreshold = StepOption(kThresholds, m_config.autoPurgeThreshold, -1);
        SaveAndApplyConfig();
        break;
    case ControlId::ThresholdNextBtn:
        m_config.autoPurgeThreshold = StepOption(kThresholds, m_config.autoPurgeThreshold, +1);
        SaveAndApplyConfig();
        break;
    case ControlId::IntervalPrevBtn:
        m_config.autoPurgeIntervalMinutes = StepOption(kIntervals, m_config.autoPurgeIntervalMinutes, -1);
        SaveAndApplyConfig();
        break;
    case ControlId::IntervalNextBtn:
        m_config.autoPurgeIntervalMinutes = StepOption(kIntervals, m_config.autoPurgeIntervalMinutes, +1);
        SaveAndApplyConfig();
        break;
    case ControlId::ToggleNotifications:
        m_config.showNotifications = !m_config.showNotifications;
        SaveAndApplyConfig();
        break;
    case ControlId::ElevateBtn:
        if (!m_isElevated && MemoryEngine::RestartElevated(false)) {
            m_closingForReal = true;
            DestroyWindow(m_hwnd);
        }
        break;
    case ControlId::SupportDevBtn:
        ShellExecuteW(nullptr, L"open", L"https://sociabuzz.com/idnvinnmalay", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    default:
        break;
    }
}

void AppWindow::StartUiAnimationLoop() {
    if (!m_animTimerActive && m_hwnd) {
        SetTimer(m_hwnd, IDT_UI_ANIM, kUiAnimMs, nullptr);
        m_animTimerActive = true;
    }
}

void AppWindow::SyncToggleTargetsFromConfig(bool immediate) {
    auto setVal = [&](ToggleId id, bool v) {
        if (immediate) {
            m_togglePos[static_cast<size_t>(id)] = v ? 1.0f : 0.0f;
        }
    };
    if (!immediate) return;
    setVal(ToggleId::MaximizeGame,   m_config.maximizeGameRam);
    setVal(ToggleId::AutoStart,      m_config.autoStart);
    setVal(ToggleId::StartMinimized, m_config.startMinimized);
    setVal(ToggleId::MinimizeToTray, m_config.minimizeToTray);
    setVal(ToggleId::CloseToTray,    m_config.closeToTray);
    setVal(ToggleId::AlwaysOnTop,    m_config.alwaysOnTop);
    setVal(ToggleId::WorkingSets,    m_config.purgeWorkingSets);
    setVal(ToggleId::SystemCache,    m_config.purgeSystemCache);
    setVal(ToggleId::StandbyList,    m_config.purgeStandbyList);
    setVal(ToggleId::ModifiedList,   m_config.purgeModifiedList);
    setVal(ToggleId::AutoPurge,      m_config.autoPurgeEnabled);
    setVal(ToggleId::Notifications,  m_config.showNotifications);
}

bool AppWindow::AdvanceAnimations() {
    bool moving = false;

    const double pctDiff = m_targetPercent - m_displayedPercent;
    if (std::fabs(pctDiff) <= 0.20) {
        m_displayedPercent = m_targetPercent;
    } else {
        m_displayedPercent += pctDiff * 0.22;
        moving = true;
    }

    if (m_sloganAnimT < m_sloganAnimTarget) {
        m_sloganAnimT = std::min(m_sloganAnimTarget, m_sloganAnimT + 0.045f);
        moving = true;
    }

    if (m_viewTransT < 1.0f) {
        m_viewTransT = std::min(1.0f, m_viewTransT + 0.085f);
        moving = true;
    }

    const float gearTarget = (m_hoverControl.id == ControlId::SettingsBtn) ? 45.0f : 0.0f;
    m_gearRotDeg = SmoothApproach(m_gearRotDeg, gearTarget, 0.24f, moving);

    if (m_purgeRunning.load()) {
        m_purgePulsePhase += 0.017f;
        if (m_purgePulsePhase >= 1.0f) m_purgePulsePhase -= 1.0f;
        moving = true;
    } else if (m_purgePulsePhase != 0.0f) {
        m_purgePulsePhase = 0.0f;
    }

    const bool targets[static_cast<size_t>(ToggleId::Count)] = {
        m_config.maximizeGameRam,
        m_config.autoStart,
        m_config.startMinimized,
        m_config.minimizeToTray,
        m_config.closeToTray,
        m_config.alwaysOnTop,
        m_config.purgeWorkingSets,
        m_config.purgeSystemCache,
        m_config.purgeStandbyList,
        m_config.purgeModifiedList,
        m_config.autoPurgeEnabled,
        m_config.showNotifications
    };
    for (size_t i = 0; i < m_togglePos.size(); ++i) {
        m_togglePos[i] = SmoothApproach(m_togglePos[i], targets[i] ? 1.0f : 0.0f, 0.26f, moving);
    }

    return moving;
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

void AppWindow::SaveAndApplyConfig() {
    ConfigManager::SetAutoStartup(m_config.autoStart, m_config.startMinimized);
    ConfigManager::Save(m_config);
    ApplyAlwaysOnTop();
    UpdateTrayIcon(m_lastSnapshot, true);
    RefreshGameProtectionUi(m_config.maximizeGameRam, false);

    if (!m_purgeRunning.load()) {
        UpdateIdlePurgeStatusText();
    }
    StartUiAnimationLoop();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AppWindow::SwitchToView(ActiveView view) {
    if (m_activeView == view) return;
    m_viewSlideDir = (view == ActiveView::Main) ? -1 : 1;
    m_activeView = view;
    m_viewTransT = 0.0f;
    m_hoverControl = {};
    m_tooltipVisible = false;
    KillTimer(m_hwnd, IDT_TOOLTIP_CHECK);

    if (view == ActiveView::Games) {
        RefreshGameProtectionUi(m_config.maximizeGameRam, true);
    }
    StartUiAnimationLoop();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AppWindow::UpdateIdlePurgeStatusText() {
    const bool autoActive = m_config.autoPurgeEnabled || (m_config.autoPurgeIntervalMinutes > 0);
    m_purgeStatusMsg = autoActive ? L"Auto Purge: ON" : L"Auto Purge: OFF";
    m_purgeStatusAccent = autoActive;
}

void AppWindow::ScrollBy(float deltaDip) {
    const size_t idx = static_cast<size_t>(m_activeView);
    const float next = std::clamp(m_scrollOffset[idx] + deltaDip, 0.0f, m_scrollMaxOffset[idx]);
    if (next != m_scrollOffset[idx]) {
        m_scrollOffset[idx] = next;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void AppWindow::ClampActiveScroll() {
    const size_t idx = static_cast<size_t>(m_activeView);
    m_scrollOffset[idx] = std::clamp(m_scrollOffset[idx], 0.0f, m_scrollMaxOffset[idx]);
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
            if (m_activeView == ActiveView::Main) {
                m_sloganAnimT = 0.0f;
                m_sloganAnimTarget = 1.0f;
                StartUiAnimationLoop();
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

    if (res) {
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

        m_purgeStatusMsg = msg;
        m_purgeStatusAccent = (res->freedBytes > 0);

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
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AppWindow::RefreshMemorySnapshot(bool checkAutoPurge) {
    const MemorySnapshot snap = MemoryEngine::Query();
    m_lastSnapshot = snap;

    m_targetPercent = static_cast<double>(snap.usagePercent);
    if (std::fabs(m_displayedPercent - m_targetPercent) > 0.25) {
        StartUiAnimationLoop();
    } else {
        m_displayedPercent = m_targetPercent;
    }

    UpdateTrayIcon(snap, false);

    ++m_gameBoostTickDivider;
    const bool applyBoostThisTick = m_config.maximizeGameRam && ((m_gameBoostTickDivider % 3) == 1);
    RefreshGameProtectionUi(applyBoostThisTick, false);

    if (IsWindowVisible(m_hwnd)) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

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

void AppWindow::TriggerPurgeAsync(bool fromAuto) {
    bool expected = false;
    if (!m_purgeRunning.compare_exchange_strong(expected, true)) {
        return;
    }

    KillTimer(m_hwnd, IDT_STATUS_RESET);
    KillTimer(m_hwnd, IDT_TOAST_COMPLETE);
    m_pendingToastResult.reset();

    m_purgePulsePhase = 0.0f;
    m_purgeStatusMsg = fromAuto ? L"Auto-purging..." : L"Purging...";
    m_purgeStatusAccent = false;
    StartUiAnimationLoop();
    InvalidateRect(m_hwnd, nullptr, FALSE);

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
            m_gameActionMsg = clean + L" is already in Game Shield.";
            m_gameActionAccent = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }

    m_config.protectedGames.push_back(clean);
    ConfigManager::Save(m_config);
    MemoryEngine::EnableAvailablePrivileges();
    RefreshGameProtectionUi(m_config.maximizeGameRam, true);

    m_gameActionMsg = L"Protected: " + clean + L" (excluded from purge & RAM prioritized).";
    m_gameActionAccent = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
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

    m_gameActionMsg = L"Removed " + exeName + L" from Game Shield.";
    m_gameActionAccent = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
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
    if (rebuildLists) {
        m_cachedRunningProcs = MemoryEngine::ListRunningProcesses();
        ClampActiveScroll();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
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
    wcscpy_s(nid.szTip, L"VRam V2.1");

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
                 L"VRam V2.1 - RAM: %u%% (%s / %s)",
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
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, visible ? L"Hide to Tray" : L"Show VRam V2.1");
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
