#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vram {

class TrayIconRenderer {
public:
    TrayIconRenderer();
    ~TrayIconRenderer();

    TrayIconRenderer(const TrayIconRenderer&) = delete;
    TrayIconRenderer& operator=(const TrayIconRenderer&) = delete;

    HICON CreateLogoIcon(HINSTANCE hInst, UINT dpi);
    HICON CreateUsagePercentIcon(uint32_t percent, UINT dpi);

    static std::wstring FindExternalLogoSvgPath();
    static std::vector<uint8_t> LoadLogoSvgBytes(HINSTANCE hInst);
    static std::vector<uint8_t> LoadLogoPngBytes(HINSTANCE hInst);

private:
    HICON BitmapPixelsToIcon(const uint8_t* bgraPixels, int width, int height);
    HICON RenderSvgToIcon(const std::vector<uint8_t>& svgBytes, int sizePx);
};

}
