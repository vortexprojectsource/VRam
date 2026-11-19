#ifdef WINVER
#undef WINVER
#endif
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#ifdef NTDDI_VERSION
#undef NTDDI_VERSION
#endif
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000004

#include "TrayIconRenderer.hpp"
#include "Win32Compat.hpp"
#include "../resources/resource.h"

#include <commctrl.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace vram {

namespace {

int GetTrayIconSizePx(UINT dpi) {
    return compat::GetSystemMetricsForDpiCompat(SM_CXSMICON, dpi);
}

HICON CreateGdiUsagePercentIcon(uint32_t clamped, int sizePx, HICON(*bitmapToIconFn)(const uint8_t*, int, int)) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sizePx;
    bi.bmiHeader.biHeight = -sizePx;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!hdcMem || !hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        if (hdcMem) DeleteDC(hdcMem);
        return nullptr;
    }

    HGDIOBJ oldBmp = SelectObject(hdcMem, hBmp);
    RECT rc{0, 0, sizePx, sizePx};
    HBRUSH bgBrush = CreateSolidBrush(RGB(244, 244, 245));
    FillRect(hdcMem, &rc, bgBrush);
    DeleteObject(bgBrush);

    HBRUSH accentBrush = CreateSolidBrush(clamped >= 90 ? RGB(220, 38, 38) : RGB(109, 40, 217));
    const int barH = std::max(2, sizePx / 6);
    const int barW = static_cast<int>((sizePx - 2) * (static_cast<double>(clamped) / 100.0));
    RECT barRc{1, sizePx - barH - 1, 1 + std::max(1, barW), sizePx - 1};
    FillRect(hdcMem, &barRc, accentBrush);
    DeleteObject(accentBrush);

    const int fontPx = (clamped >= 100) ? std::max(7, (sizePx * 45) / 100) : std::max(8, (sizePx * 56) / 100);
    HFONT hFont = CreateFontW(-fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = hFont ? SelectObject(hdcMem, hFont) : nullptr;
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(24, 24, 27));
    wchar_t numBuf[8]{};
    _snwprintf_s(numBuf, _TRUNCATE, L"%u", clamped);
    RECT txtRc{0, 0, sizePx, sizePx - barH};
    DrawTextW(hdcMem, numBuf, -1, &txtRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldFont) SelectObject(hdcMem, oldFont);
    if (hFont) DeleteObject(hFont);
    GdiFlush();

    auto* px = static_cast<uint8_t*>(bits);
    const size_t totalPx = static_cast<size_t>(sizePx) * sizePx;
    for (size_t i = 0; i < totalPx; ++i) {
        px[i * 4 + 3] = 255;
    }

    SelectObject(hdcMem, oldBmp);
    HICON out = bitmapToIconFn(px, sizePx, sizePx);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    return out;
}

}

TrayIconRenderer::TrayIconRenderer() = default;
TrayIconRenderer::~TrayIconRenderer() = default;

std::wstring TrayIconRenderer::FindExternalLogoSvgPath() {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    PathRemoveFileSpecW(exePath);

    std::wstring candidate = std::wstring(exePath) + L"\\logo.svg";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return candidate;
    }

    std::wstring assetCandidate = std::wstring(exePath) + L"\\assets\\icons\\logo.svg";
    if (GetFileAttributesW(assetCandidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return assetCandidate;
    }

    return {};
}

std::vector<uint8_t> TrayIconRenderer::LoadLogoSvgBytes(HINSTANCE hInst) {
    const std::wstring extPath = FindExternalLogoSvgPath();
    if (!extPath.empty()) {
        HANDLE hFile = CreateFileW(extPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(hFile, &sz) && sz.QuadPart > 0 && sz.QuadPart < 2 * 1024 * 1024) {
                std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart));
                DWORD readBytes = 0;
                if (ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &readBytes, nullptr) &&
                    readBytes == buf.size()) {
                    CloseHandle(hFile);
                    return buf;
                }
            }
            CloseHandle(hFile);
        }
    }

    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_SVG_LOGO), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hData = LoadResource(hInst, hRes);
    if (!hData) return {};
    const DWORD size = SizeofResource(hInst, hRes);
    const void* ptr = LockResource(hData);
    if (!ptr || size == 0) return {};

    const auto* bytes = static_cast<const uint8_t*>(ptr);
    return std::vector<uint8_t>(bytes, bytes + size);
}

std::vector<uint8_t> TrayIconRenderer::LoadLogoPngBytes(HINSTANCE hInst) {
    wchar_t exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        PathRemoveFileSpecW(exePath);
        std::wstring extPng = std::wstring(exePath) + L"\\logo.png";
        HANDLE hFile = CreateFileW(extPng.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(hFile, &sz) && sz.QuadPart > 0 && sz.QuadPart < 10 * 1024 * 1024) {
                std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart));
                DWORD readBytes = 0;
                if (ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &readBytes, nullptr) &&
                    readBytes == buf.size()) {
                    CloseHandle(hFile);
                    return buf;
                }
            }
            CloseHandle(hFile);
        }
    }

    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_PNG_LOGO), RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hData = LoadResource(hInst, hRes);
    if (!hData) return {};
    const DWORD size = SizeofResource(hInst, hRes);
    const void* ptr = LockResource(hData);
    if (!ptr || size == 0) return {};

    const auto* bytes = static_cast<const uint8_t*>(ptr);
    return std::vector<uint8_t>(bytes, bytes + size);
}

HICON TrayIconRenderer::BitmapPixelsToIcon(const uint8_t* bgraPixels, int width, int height) {
    if (!bgraPixels || width <= 0 || height <= 0) {
        return nullptr;
    }

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = width;
    bi.bV5Height = -height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdcScreen = GetDC(nullptr);
    void* dibBits = nullptr;
    HBITMAP hColor = CreateDIBSection(hdcScreen, reinterpret_cast<BITMAPINFO*>(&bi),
                                      DIB_RGB_COLORS, &dibBits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!hColor || !dibBits) {
        if (hColor) DeleteObject(hColor);
        return nullptr;
    }

    memcpy(dibBits, bgraPixels, static_cast<size_t>(width) * height * 4);

    HBITMAP hMask = CreateBitmap(width, height, 1, 1, nullptr);
    if (!hMask) {
        DeleteObject(hColor);
        return nullptr;
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;

    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hMask);
    DeleteObject(hColor);
    return hIcon;
}

HICON TrayIconRenderer::RenderSvgToIcon(const std::vector<uint8_t>& svgBytes, int sizePx) {
    if (svgBytes.empty() || sizePx <= 0) {
        return nullptr;
    }

    ComPtr<ID3D11Device> d3dDevice;
    D3D_FEATURE_LEVEL fl{};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &d3dDevice, &fl, nullptr);
    if (FAILED(hr)) return nullptr;

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID2D1Factory6> d2dFactory;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory6),
                           nullptr, reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
    if (FAILED(hr)) return nullptr;

    ComPtr<ID2D1Device5> d2dDevice;
    hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    if (FAILED(hr)) return nullptr;

    ComPtr<ID2D1DeviceContext5> dc;
    hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
    if (FAILED(hr)) return nullptr;

    D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ComPtr<ID2D1Bitmap1> targetBmp;
    hr = dc->CreateBitmap(D2D1::SizeU(sizePx, sizePx), nullptr, 0, &targetProps, &targetBmp);
    if (FAILED(hr)) return nullptr;

    D2D1_BITMAP_PROPERTIES1 readProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ComPtr<ID2D1Bitmap1> readBmp;
    hr = dc->CreateBitmap(D2D1::SizeU(sizePx, sizePx), nullptr, 0, &readProps, &readBmp);
    if (FAILED(hr)) return nullptr;

    ComPtr<IStream> svgStream;
    svgStream.Attach(SHCreateMemStream(svgBytes.data(), static_cast<UINT>(svgBytes.size())));
    if (!svgStream) return nullptr;

    ComPtr<ID2D1SvgDocument> svgDoc;
    hr = dc->CreateSvgDocument(svgStream.Get(), D2D1::SizeF(static_cast<float>(sizePx), static_cast<float>(sizePx)), &svgDoc);
    if (FAILED(hr) || !svgDoc) return nullptr;

    ComPtr<ID2D1SvgElement> root;
    svgDoc->GetRoot(&root);
    if (root) {
        D2D1_SVG_VIEWBOX vb{};
        if (SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) &&
            vb.width > 0.0f && vb.height > 0.0f) {
            const float scale = std::min(static_cast<float>(sizePx) / vb.width,
                                         static_cast<float>(sizePx) / vb.height);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(-vb.x * scale, -vb.y * scale));
        }
    }

    dc->SetTarget(targetBmp.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawSvgDocument(svgDoc.Get());
    hr = dc->EndDraw();
    if (FAILED(hr)) return nullptr;

    D2D1_POINT_2U dstPt{0, 0};
    D2D1_RECT_U srcRect{0, 0, static_cast<UINT32>(sizePx), static_cast<UINT32>(sizePx)};
    hr = readBmp->CopyFromBitmap(&dstPt, targetBmp.Get(), &srcRect);
    if (FAILED(hr)) return nullptr;

    D2D1_MAPPED_RECT mapped{};
    hr = readBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr) || !mapped.bits || mapped.pitch < static_cast<UINT>(sizePx) * 4) {
        if (SUCCEEDED(hr)) readBmp->Unmap();
        return nullptr;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(sizePx) * sizePx * 4);
    for (int y = 0; y < sizePx; ++y) {
        memcpy(pixels.data() + static_cast<size_t>(y) * sizePx * 4,
               mapped.bits + static_cast<size_t>(y) * mapped.pitch,
               static_cast<size_t>(sizePx) * 4);
    }
    readBmp->Unmap();

    return BitmapPixelsToIcon(pixels.data(), sizePx, sizePx);
}

HICON TrayIconRenderer::CreateLogoIcon(HINSTANCE hInst, UINT dpi) {
    const int sizePx = GetTrayIconSizePx(dpi);

    if (!FindExternalLogoSvgPath().empty()) {
        auto svgBytes = LoadLogoSvgBytes(hInst);
        if (HICON hSvgIcon = RenderSvgToIcon(svgBytes, sizePx)) {
            return hSvgIcon;
        }
    }

    HICON hIco = nullptr;
    if (SUCCEEDED(LoadIconWithScaleDown(hInst, MAKEINTRESOURCEW(IDI_VRAM_APP), sizePx, sizePx, &hIco)) && hIco) {
        return hIco;
    }
    return static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_VRAM_APP), IMAGE_ICON, sizePx, sizePx, LR_DEFAULTCOLOR));
}

HICON TrayIconRenderer::CreateUsagePercentIcon(uint32_t percent, UINT dpi) {
    const int sizePx = std::max(16, GetTrayIconSizePx(dpi));
    const uint32_t clamped = std::min<uint32_t>(percent, 100);

    auto gdiFallback = [&]() -> HICON {
        return CreateGdiUsagePercentIcon(clamped, sizePx, &TrayIconRenderer::BitmapPixelsToIcon);
    };

    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr) || !wicFactory) return gdiFallback();

    ComPtr<IWICBitmap> wicBmp;
    hr = wicFactory->CreateBitmap(sizePx, sizePx, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand, &wicBmp);
    if (FAILED(hr) || !wicBmp) return gdiFallback();

    ComPtr<ID2D1Factory> d2dFactory;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
    if (FAILED(hr) || !d2dFactory) return gdiFallback();

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ComPtr<ID2D1RenderTarget> rt;
    hr = d2dFactory->CreateWicBitmapRenderTarget(wicBmp.Get(), rtProps, &rt);
    if (FAILED(hr) || !rt) return gdiFallback();

    ComPtr<IDWriteFactory> dwriteFactory;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
    if (FAILED(hr) || !dwriteFactory) return gdiFallback();

    const float s = static_cast<float>(sizePx);
    const float fontSize = (clamped >= 100) ? (s * 0.44f) : (s * 0.56f);

    ComPtr<IDWriteTextFormat> textFmt;
    hr = dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr,
                                         DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_CONDENSED, fontSize, L"en-us", &textFmt);
    if (FAILED(hr) || !textFmt) return gdiFallback();

    textFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    textFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    D2D1_COLOR_F accent = D2D1::ColorF(0x6D28D9);
    if (clamped >= 90) {
        accent = D2D1::ColorF(0xDC2626);
    }

    ComPtr<ID2D1SolidColorBrush> bgBrush;
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0xF4F4F5, 1.0f), &bgBrush)) || !bgBrush ||
        FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0xD4D4D8, 1.0f), &borderBrush)) || !borderBrush ||
        FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0x18181B, 1.0f), &textBrush)) || !textBrush ||
        FAILED(rt->CreateSolidColorBrush(accent, &accentBrush)) || !accentBrush) {
        return gdiFallback();
    }

    const float radius = std::max(2.0f, s * 0.18f);
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, s - 0.5f, s - 0.5f), radius, radius);

    wchar_t numBuf[8]{};
    _snwprintf_s(numBuf, _TRUNCATE, L"%u", clamped);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->FillRoundedRectangle(rr, bgBrush.Get());
    rt->DrawRoundedRectangle(rr, borderBrush.Get(), 1.0f);

    const float barH = std::max(2.0f, s * 0.14f);
    const float barPad = std::max(2.0f, s * 0.12f);
    const float fillRatio = static_cast<float>(clamped) / 100.0f;
    const float barW = (s - 2.0f * barPad) * fillRatio;
    rt->FillRectangle(D2D1::RectF(barPad, s - barH - 1.5f, barPad + barW, s - 1.5f), accentBrush.Get());

    const D2D1_RECT_F textRect = D2D1::RectF(0.0f, -barH * 0.35f, s, s - barH);
    rt->DrawTextW(numBuf, static_cast<UINT32>(wcslen(numBuf)), textFmt.Get(), textRect, textBrush.Get());
    hr = rt->EndDraw();
    if (FAILED(hr)) return gdiFallback();

    ComPtr<IWICBitmapLock> lock;
    WICRect lockRect{0, 0, sizePx, sizePx};
    hr = wicBmp->Lock(&lockRect, WICBitmapLockRead, &lock);
    if (FAILED(hr) || !lock) return gdiFallback();

    UINT bufSize = 0;
    UINT stride = 0;
    BYTE* ptr = nullptr;
    if (FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&bufSize, &ptr)) ||
        !ptr ||
        stride < static_cast<UINT>(sizePx) * 4 ||
        bufSize < stride * static_cast<UINT>(sizePx)) {
        return gdiFallback();
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(sizePx) * sizePx * 4);
    for (int y = 0; y < sizePx; ++y) {
        memcpy(pixels.data() + static_cast<size_t>(y) * sizePx * 4,
               ptr + static_cast<size_t>(y) * stride,
               static_cast<size_t>(sizePx) * 4);
    }

    return BitmapPixelsToIcon(pixels.data(), sizePx, sizePx);
}

}
