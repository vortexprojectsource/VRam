#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vram::compat {

/// Returns system DPI cleanly on Windows 7 through Windows 11.
inline UINT GetDpiForSystemCompat() {
    using PfnGetDpiForSystem = UINT(WINAPI*)();
    static const auto pfn = reinterpret_cast<PfnGetDpiForSystem>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (pfn) {
        const UINT dpi = pfn();
        if (dpi > 0) return dpi;
    }
    HDC hdc = GetDC(nullptr);
    int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(nullptr, hdc);
    return (dpi > 0) ? static_cast<UINT>(dpi) : 96u;
}

/// Returns per-window DPI on Windows 10 1607+, falling back to system DPI on older OS builds.
inline UINT GetDpiForWindowCompat(HWND hwnd) {
    if (hwnd) {
        using PfnGetDpiForWindow = UINT(WINAPI*)(HWND);
        static const auto pfn = reinterpret_cast<PfnGetDpiForWindow>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (pfn) {
            const UINT dpi = pfn(hwnd);
            if (dpi > 0) return dpi;
        }
    }
    return GetDpiForSystemCompat();
}

/// Returns DPI-scaled system metrics on Windows 10 1607+, falling back on older builds.
inline int GetSystemMetricsForDpiCompat(int index, UINT dpi) {
    const UINT safeDpi = (dpi == 0) ? 96u : dpi;
    using PfnGetSystemMetricsForDpi = int(WINAPI*)(int, UINT);
    static const auto pfn = reinterpret_cast<PfnGetSystemMetricsForDpi>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    if (pfn) {
        const int v = pfn(index, safeDpi);
        if (v > 0) return v;
    }
    const UINT sysDpi = GetDpiForSystemCompat();
    return std::max(1, MulDiv(GetSystemMetrics(index), static_cast<int>(safeDpi), static_cast<int>(sysDpi)));
}

/// Scales DIP coordinates symmetrically using explicit DPI.
inline int ScaleDip(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi == 0 ? 96u : dpi), 96);
}

/// Decodes raw PNG bytes into premultiplied 32-bit PBGRA pixels scaled to targetHeightPx.
inline bool DecodePngToPbgra(const std::vector<uint8_t>& pngBytes,
                             UINT targetHeightPx,
                             uint32_t& outW,
                             uint32_t& outH,
                             std::vector<uint8_t>& outPbgra) {
    outW = 0;
    outH = 0;
    outPbgra.clear();
    if (pngBytes.empty() || targetHeightPx == 0) return false;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wicFactory))) || !wicFactory) {
        return false;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(pngBytes.data(), static_cast<UINT>(pngBytes.size())));
    if (!stream) return false;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder) {
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return false;

    UINT w = 0;
    UINT h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    const UINT targetW = std::max<UINT>(
        1, static_cast<UINT>(std::lround(static_cast<double>(w) * targetHeightPx / static_cast<double>(h))));

    Microsoft::WRL::ComPtr<IWICBitmapSource> source = frame;
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    if (SUCCEEDED(wicFactory->CreateBitmapScaler(&scaler)) && scaler &&
        SUCCEEDED(scaler->Initialize(frame.Get(), targetW, targetHeightPx, WICBitmapInterpolationModeFant))) {
        source = scaler;
        w = targetW;
        h = targetHeightPx;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory->CreateFormatConverter(&converter)) || !converter) return false;
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const UINT stride = w * 4;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * h);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data()))) {
        return false;
    }

    outW = w;
    outH = h;
    outPbgra = std::move(buf);
    return true;
}

}
