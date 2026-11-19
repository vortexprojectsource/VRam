#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "MemoryEngine.hpp"

namespace vram {

class PurgeToastWindow {
public:
    PurgeToastWindow() = default;
    ~PurgeToastWindow();

    PurgeToastWindow(const PurgeToastWindow&) = delete;
    PurgeToastWindow& operator=(const PurgeToastWindow&) = delete;

    bool Create(HINSTANCE hInstance, HWND ownerNotifyHwnd, UINT openAppMsg);
    void Destroy();

    void ShowPurging(const MemorySnapshot& beforeSnap, const ProtectedGameStatus& gameStatus);
    void ShowCompleted(const PurgeResult& result);
    void HideImmediate();
    bool IsVisible() const;

private:
    enum class ToastPhase {
        Hidden,
        Running,
        Completed,
        FadingOut
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void PositionBottomRight();
    void LoadCachedLogoPixels();
    void RenderLayered();
    void RenderGdiFallback(HDC hdcMem, int widthPx, int heightPx);

    static constexpr int kBaseWidthDip = 356;
    static constexpr int kBaseHeightDip = 118;
    static constexpr uint32_t kAutoCloseDurationMs = 3000;

    static constexpr UINT_PTR kTimerAnimation = 201;

    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_ownerNotifyHwnd = nullptr;
    UINT m_openAppMsg = 0;
    UINT m_dpi = 96;

    uint32_t m_logoW = 0;
    uint32_t m_logoH = 0;
    std::vector<uint8_t> m_logoPbgra;

    ToastPhase m_phase = ToastPhase::Hidden;
    BYTE m_alpha = 255;
    float m_spinnerDeg = 0.0f;
    float m_barPhase = 0.0f;
    uint64_t m_completedTickMs = 0;
    uint32_t m_remainingAutoCloseMs = kAutoCloseDurationMs;
    bool m_hovering = false;
    bool m_hoverCloseX = false;

    MemorySnapshot m_runningBefore{};
    ProtectedGameStatus m_runningGame{};
    PurgeResult m_completedResult{};

    RECT m_rectCloseX{};
};

}
