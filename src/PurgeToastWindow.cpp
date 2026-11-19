#include "PurgeToastWindow.hpp"
#include "TrayIconRenderer.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace vram {

namespace {

constexpr wchar_t kToastClassName[] = L"VRam_V2_PurgeToastWindow";
constexpr float kPi = 3.14159265358979323846f;

int ScalePx(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi == 0 ? 96u : dpi), 96);
}

}

void PurgeToastWindow::LoadCachedLogoPixels() {
    if (!m_hInstance || !m_logoPbgra.empty()) return;
    auto pngBytes = TrayIconRenderer::LoadLogoPngBytes(m_hInstance);
    if (pngBytes.empty()) return;

    ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wicFactory))) || !wicFactory) {
        return;
    }

    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(pngBytes.data(), static_cast<UINT>(pngBytes.size())));
    if (!stream) return;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder) {
        return;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return;

    const UINT targetH = 36;
    const UINT targetW = std::max<UINT>(1, static_cast<UINT>(std::lround(static_cast<double>(w) * targetH / static_cast<double>(h))));

    ComPtr<IWICBitmapSource> scaledSource = frame;
    ComPtr<IWICBitmapScaler> scaler;
    if (SUCCEEDED(wicFactory->CreateBitmapScaler(&scaler)) && scaler &&
        SUCCEEDED(scaler->Initialize(frame.Get(), targetW, targetH, WICBitmapInterpolationModeFant))) {
        scaledSource = scaler;
        w = targetW;
        h = targetH;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory->CreateFormatConverter(&converter)) || !converter) return;
    if (FAILED(converter->Initialize(scaledSource.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom))) {
        return;
    }

    const UINT stride = w * 4;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * h);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(buf.size()), buf.data()))) {
        return;
    }

    m_logoW = w;
    m_logoH = h;
    m_logoPbgra = std::move(buf);
}

PurgeToastWindow::~PurgeToastWindow() {
    Destroy();
}

bool PurgeToastWindow::Create(HINSTANCE hInstance, HWND ownerNotifyHwnd, UINT openAppMsg) {
    if (m_hwnd) {
        return true;
    }

    m_hInstance = hInstance;
    m_ownerNotifyHwnd = ownerNotifyHwnd;
    m_openAppMsg = openAppMsg;
    LoadCachedLogoPixels();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PurgeToastWindow::WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kToastClassName;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kToastClassName,
        L"VRam V2.0 Notification",
        WS_POPUP,
        0, 0,
        ScalePx(kBaseWidthDip, 96), ScalePx(kBaseHeightDip, 96),
        nullptr,
        nullptr,
        hInstance,
        this
    );

    if (!m_hwnd) {
        return false;
    }

    m_dpi = GetDpiForWindow(m_hwnd);
    if (m_dpi == 0) {
        m_dpi = 96;
    }
    return true;
}

void PurgeToastWindow::Destroy() {
    if (m_hwnd) {
        KillTimer(m_hwnd, kTimerAnimation);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool PurgeToastWindow::IsVisible() const {
    return m_hwnd && m_phase != ToastPhase::Hidden && IsWindowVisible(m_hwnd);
}

void PurgeToastWindow::ShowPurging(const MemorySnapshot& beforeSnap, const ProtectedGameStatus& gameStatus) {
    if (!m_hwnd && m_hInstance) {
        Create(m_hInstance, m_ownerNotifyHwnd, m_openAppMsg);
    }
    if (!m_hwnd) return;

    m_runningBefore = beforeSnap;
    m_runningGame = gameStatus;
    m_phase = ToastPhase::Running;
    m_alpha = 255;
    m_spinnerDeg = 0.0f;
    m_barPhase = 0.0f;
    m_remainingAutoCloseMs = kAutoCloseDurationMs;

    SetTimer(m_hwnd, kTimerAnimation, 33, nullptr);
    PositionBottomRight();
    RenderLayered();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PurgeToastWindow::ShowCompleted(const PurgeResult& result) {
    if (!m_hwnd && m_hInstance) {
        Create(m_hInstance, m_ownerNotifyHwnd, m_openAppMsg);
    }
    if (!m_hwnd) return;

    m_completedResult = result;
    m_phase = ToastPhase::Completed;
    m_alpha = 255;
    m_remainingAutoCloseMs = kAutoCloseDurationMs;
    m_completedTickMs = GetTickCount64();

    SetTimer(m_hwnd, kTimerAnimation, 33, nullptr);
    PositionBottomRight();
    RenderLayered();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PurgeToastWindow::HideImmediate() {
    if (!m_hwnd) return;
    KillTimer(m_hwnd, kTimerAnimation);
    m_phase = ToastPhase::Hidden;
    m_alpha = 0;
    ShowWindow(m_hwnd, SW_HIDE);
}

void PurgeToastWindow::PositionBottomRight() {
    if (!m_hwnd) return;

    RECT workArea{};
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR hMon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
        workArea = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    }

    const UINT curDpi = GetDpiForWindow(m_hwnd);
    if (curDpi > 0) {
        m_dpi = curDpi;
    }

    const int widthPx = ScalePx(kBaseWidthDip, m_dpi);
    const int heightPx = ScalePx(kBaseHeightDip, m_dpi);
    const int margin = ScalePx(14, m_dpi);

    const int x = workArea.right - widthPx - margin;
    const int y = workArea.bottom - heightPx - margin;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, widthPx, heightPx, SWP_NOACTIVATE);
}

void PurgeToastWindow::RenderLayered() {
    if (!m_hwnd || m_phase == ToastPhase::Hidden) return;

    const int widthPx = ScalePx(kBaseWidthDip, m_dpi);
    const int heightPx = ScalePx(kBaseHeightDip, m_dpi);
    if (widthPx <= 0 || heightPx <= 0) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = widthPx;
    bi.bmiHeader.biHeight = -heightPx;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hdcMem || !hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        if (hdcMem) DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(hdcMem, hBmp);

    bool renderedD2D = false;
    const float w = static_cast<float>(kBaseWidthDip);
    const float h = static_cast<float>(kBaseHeightDip);

    m_rectCloseX = RECT{
        ScalePx(kBaseWidthDip - 34, m_dpi),
        ScalePx(6, m_dpi),
        ScalePx(kBaseWidthDip - 6, m_dpi),
        ScalePx(32, m_dpi)
    };

    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<IWICBitmap> wicBmp;
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<IDWriteFactory> dwriteFactory;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wicFactory))) && wicFactory &&
        SUCCEEDED(wicFactory->CreateBitmap(widthPx, heightPx, GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapCacheOnDemand, &wicBmp)) && wicBmp &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf())) && d2dFactory) {

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(m_dpi),
            static_cast<float>(m_dpi)
        );

        if (SUCCEEDED(d2dFactory->CreateWicBitmapRenderTarget(wicBmp.Get(), props, &rt)) && rt &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()))) && dwriteFactory) {

            ComPtr<IDWriteTextFormat> fmtBrand;
            ComPtr<IDWriteTextFormat> fmtPill;
            ComPtr<IDWriteTextFormat> fmtTitle;
            ComPtr<IDWriteTextFormat> fmtSub;
            ComPtr<IDWriteTextFormat> fmtShield;

            dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &fmtBrand);
            dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &fmtPill);
            dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &fmtTitle);
            dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &fmtSub);
            dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.8f, L"en-us", &fmtShield);

            if (fmtPill) {
                fmtPill->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmtPill->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }

            const bool isRunning = (m_phase == ToastPhase::Running);

            // VRam Native Signature Theme: Clean White (#FFFFFF), Violet Purple (#6D28D9), Ink Black (#111113)
            ComPtr<ID2D1SolidColorBrush> brWhiteBg;
            ComPtr<ID2D1SolidColorBrush> brBorder;
            ComPtr<ID2D1SolidColorBrush> brDivider;
            ComPtr<ID2D1SolidColorBrush> brPurple;
            ComPtr<ID2D1SolidColorBrush> brLavender;
            ComPtr<ID2D1SolidColorBrush> brBlack;
            ComPtr<ID2D1SolidColorBrush> brMuted;
            ComPtr<ID2D1SolidColorBrush> brWhiteText;
            ComPtr<ID2D1SolidColorBrush> brCloseX;
            ComPtr<ID2D1SolidColorBrush> brCloseHover;

            rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 1.0f), &brWhiteBg);
            rt->CreateSolidColorBrush(D2D1::ColorF(0xE4E4E7, 1.0f), &brBorder);
            rt->CreateSolidColorBrush(D2D1::ColorF(0xF1F1F5, 1.0f), &brDivider);
            rt->CreateSolidColorBrush(D2D1::ColorF(0x6D28D9, 1.0f), &brPurple);
            rt->CreateSolidColorBrush(D2D1::ColorF(0xF3EEFF, 1.0f), &brLavender);
            rt->CreateSolidColorBrush(D2D1::ColorF(0x111113, 1.0f), &brBlack);
            rt->CreateSolidColorBrush(D2D1::ColorF(0x52525B, 1.0f), &brMuted);
            rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 1.0f), &brWhiteText);
            rt->CreateSolidColorBrush(m_hoverCloseX ? D2D1::ColorF(0x111113, 1.0f) : D2D1::ColorF(0x71717A, 1.0f), &brCloseX);
            rt->CreateSolidColorBrush(D2D1::ColorF(0xF4F4F6, 1.0f), &brCloseHover);

            ComPtr<ID2D1Bitmap> logoBmp;
            if (!m_logoPbgra.empty() && m_logoW > 0 && m_logoH > 0) {
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
                );
                rt->CreateBitmap(D2D1::SizeU(m_logoW, m_logoH), m_logoPbgra.data(), m_logoW * 4, bp, &logoBmp);
            }

            ComPtr<ID2D1RoundedRectangleGeometry> cardClip;
            const D2D1_ROUNDED_RECT outerCard = D2D1::RoundedRect(D2D1::RectF(1.0f, 1.0f, w - 1.0f, h - 1.0f), 11.0f, 11.0f);
            d2dFactory->CreateRoundedRectangleGeometry(outerCard, &cardClip);

            if (brWhiteBg && brBorder && brDivider && brPurple && brLavender && brBlack && brMuted &&
                brWhiteText && brCloseX && fmtBrand && fmtPill && fmtTitle && fmtSub && fmtShield && cardClip) {

                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0, 0, 0, 0));

                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), cardClip.Get()), nullptr);

                // Clean white card + crisp 3.5px bottom progress track
                rt->FillRectangle(D2D1::RectF(0, 0, w, h), brWhiteBg.Get());

                // Bottom progress / status strip
                const float stripY = h - 4.0f;
                rt->FillRectangle(D2D1::RectF(0.0f, stripY, w, h), brLavender.Get());
                if (isRunning) {
                    const float segW = w * 0.34f;
                    const float x0 = -segW + (w + segW) * m_barPhase;
                    rt->FillRectangle(D2D1::RectF(x0, stripY, x0 + segW, h), brPurple.Get());
                } else {
                    float ratio = 0.0f;
                    if (m_phase == ToastPhase::Completed && kAutoCloseDurationMs > 0) {
                        const uint64_t elapsed = m_hovering ? 0 : (GetTickCount64() - m_completedTickMs);
                        const uint32_t rem = (m_remainingAutoCloseMs > elapsed)
                            ? static_cast<uint32_t>(m_remainingAutoCloseMs - elapsed)
                            : 0u;
                        ratio = std::clamp(static_cast<float>(rem) / static_cast<float>(kAutoCloseDurationMs), 0.0f, 1.0f);
                    }
                    rt->FillRectangle(D2D1::RectF(0.0f, stripY, w * ratio, h), brPurple.Get());
                }

                // Header hairline
                rt->DrawLine(D2D1::Point2F(16.0f, 34.0f), D2D1::Point2F(w - 16.0f, 34.0f), brDivider.Get(), 1.0f);

                rt->PopLayer();
                rt->DrawRoundedRectangle(outerCard, brBorder.Get(), 1.0f);

                // Header: VR logo bitmap (or fallback dot) + "VRam" + status pill + close X
                float nextX = 16.0f;
                if (logoBmp) {
                    const D2D1_SIZE_F bmpSz = logoBmp->GetSize();
                    const float drawH = 14.0f;
                    const float drawW = (bmpSz.height > 0.0f) ? (drawH * bmpSz.width / bmpSz.height) : 26.0f;
                    rt->DrawBitmap(logoBmp.Get(), D2D1::RectF(nextX, 11.0f, nextX + drawW, 11.0f + drawH));
                    nextX += drawW + 7.0f;
                }
                rt->DrawTextW(L"VRam", 4, fmtBrand.Get(), D2D1::RectF(nextX, 10.0f, nextX + 42.0f, 27.0f), brBlack.Get());
                nextX += 42.0f;

                const wchar_t* pillStr = isRunning ? L"Purging" : L"Done";
                const float pillW = isRunning ? 52.0f : 44.0f;
                const D2D1_ROUNDED_RECT pillRr = D2D1::RoundedRect(D2D1::RectF(nextX, 10.0f, nextX + pillW, 26.0f), 8.0f, 8.0f);
                rt->FillRoundedRectangle(pillRr, isRunning ? brLavender.Get() : brPurple.Get());
                rt->DrawTextW(pillStr, static_cast<UINT32>(wcslen(pillStr)), fmtPill.Get(),
                              pillRr.rect, isRunning ? brPurple.Get() : brWhiteText.Get());

                // Clean top-right X button
                const float cx = w - 20.0f;
                const float cy = 18.0f;
                if (m_hoverCloseX && brCloseHover) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 10.0f, cy - 10.0f, cx + 10.0f, cy + 10.0f), 5.0f, 5.0f), brCloseHover.Get());
                }
                rt->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 4.0f), D2D1::Point2F(cx + 4.0f, cy + 4.0f), brCloseX.Get(), 1.6f);
                rt->DrawLine(D2D1::Point2F(cx + 4.0f, cy - 4.0f), D2D1::Point2F(cx - 4.0f, cy + 4.0f), brCloseX.Get(), 1.6f);

                // Left mini signature VRam Ring Gauge (same lavender/purple aesthetic as main window!)
                const float ringCx = 38.0f;
                const float ringCy = 73.0f;
                const float ringR = 17.5f;
                if (isRunning) {
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ringCx, ringCy), ringR, ringR), brLavender.Get(), 4.2f);
                    ComPtr<ID2D1PathGeometry> arcGeo;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(d2dFactory->CreatePathGeometry(&arcGeo)) && arcGeo && SUCCEEDED(arcGeo->Open(&sink)) && sink) {
                        const float startRad = m_spinnerDeg * (kPi / 180.0f);
                        const float sweepRad = 125.0f * (kPi / 180.0f);
                        const D2D1_POINT_2F p0{ringCx + ringR * std::cos(startRad), ringCy + ringR * std::sin(startRad)};
                        const D2D1_POINT_2F p1{ringCx + ringR * std::cos(startRad + sweepRad), ringCy + ringR * std::sin(startRad + sweepRad)};
                        sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddArc(D2D1::ArcSegment(p1, D2D1::SizeF(ringR, ringR), 0.0f,
                                                      D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(arcGeo.Get(), brPurple.Get(), 4.2f);
                    }
                } else {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ringCx, ringCy), ringR, ringR), brLavender.Get());
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ringCx, ringCy), ringR, ringR), brPurple.Get(), 2.2f);
                    ComPtr<ID2D1PathGeometry> chkGeo;
                    ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(d2dFactory->CreatePathGeometry(&chkGeo)) && chkGeo && SUCCEEDED(chkGeo->Open(&sink)) && sink) {
                        sink->BeginFigure(D2D1::Point2F(ringCx - 6.8f, ringCy + 0.5f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(D2D1::Point2F(ringCx - 1.5f, ringCy + 5.8f));
                        sink->AddLine(D2D1::Point2F(ringCx + 7.5f, ringCy - 5.2f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(chkGeo.Get(), brPurple.Get(), 2.8f);
                    }
                }

                // Right typography column
                const float colX = 70.0f;
                const float colW = w - colX - 16.0f;

                std::wstring titleLine;
                std::wstring detailLine;
                std::wstring shieldLine;
                bool hasActiveGame = false;

                if (isRunning) {
                    titleLine = L"Purging system memory...";
                    wchar_t d1[96]{};
                    _snwprintf_s(d1, _TRUNCATE, L"%u%% (%s)  •  Standby & cache cleanup",
                                 m_runningBefore.usagePercent,
                                 MemoryEngine::FormatBytes(m_runningBefore.usedBytes).c_str());
                    detailLine = d1;
                    if (m_runningGame.anyRunning && !m_runningGame.primaryGameName.empty()) {
                        hasActiveGame = true;
                        shieldLine = L"Game Shield: " + m_runningGame.primaryGameName + L" protected";
                    } else {
                        shieldLine = L"System-safe background trim running";
                    }
                } else {
                    wchar_t t1[96]{};
                    if (m_completedResult.freedBytes > 0) {
                        _snwprintf_s(t1, _TRUNCATE, L"Freed %s  (%u%% -> %u%%)",
                                     MemoryEngine::FormatBytes(m_completedResult.freedBytes).c_str(),
                                     m_completedResult.beforePercent,
                                     m_completedResult.afterPercent);
                    } else {
                        _snwprintf_s(t1, _TRUNCATE, L"Memory optimized (%u%% used)", m_completedResult.afterPercent);
                    }
                    titleLine = t1;

                    wchar_t d1[96]{};
                    _snwprintf_s(d1, _TRUNCATE, L"%s in RAM  •  %u background apps trimmed",
                                 MemoryEngine::FormatBytes(m_completedResult.afterUsedBytes).c_str(),
                                 m_completedResult.trimmedProcesses);
                    detailLine = d1;

                    if (m_completedResult.protectedProcesses > 0 && !m_completedResult.activeGameName.empty()) {
                        hasActiveGame = true;
                        wchar_t s1[96]{};
                        _snwprintf_s(s1, _TRUNCATE, L"Game Shield: %s (%s) maximized",
                                     m_completedResult.activeGameName.c_str(),
                                     MemoryEngine::FormatBytes(m_completedResult.activeGameRamBytes).c_str());
                        shieldLine = s1;
                    } else {
                        shieldLine = L"Standby & cache released cleanly";
                    }
                }

                rt->DrawTextW(titleLine.c_str(), static_cast<UINT32>(titleLine.size()), fmtTitle.Get(),
                              D2D1::RectF(colX, 44.0f, colX + colW, 64.0f), brBlack.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                rt->DrawTextW(detailLine.c_str(), static_cast<UINT32>(detailLine.size()), fmtSub.Get(),
                              D2D1::RectF(colX, 65.0f, colX + colW, 82.0f), brMuted.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                rt->DrawTextW(shieldLine.c_str(), static_cast<UINT32>(shieldLine.size()), fmtShield.Get(),
                              D2D1::RectF(colX, 84.0f, colX + colW, 102.0f),
                              hasActiveGame ? brPurple.Get() : brMuted.Get(),
                              D2D1_DRAW_TEXT_OPTIONS_CLIP);

                if (SUCCEEDED(rt->EndDraw())) {
                    ComPtr<IWICBitmapLock> lock;
                    WICRect lockRect{0, 0, widthPx, heightPx};
                    if (SUCCEEDED(wicBmp->Lock(&lockRect, WICBitmapLockRead, &lock)) && lock) {
                        UINT stride = 0;
                        UINT bufSize = 0;
                        BYTE* src = nullptr;
                        if (SUCCEEDED(lock->GetStride(&stride)) &&
                            SUCCEEDED(lock->GetDataPointer(&bufSize, &src)) &&
                            src && stride >= static_cast<UINT>(widthPx) * 4 &&
                            bufSize >= stride * static_cast<UINT>(heightPx)) {
                            for (int y = 0; y < heightPx; ++y) {
                                memcpy(static_cast<uint8_t*>(bits) + static_cast<size_t>(y) * widthPx * 4,
                                       src + static_cast<size_t>(y) * stride,
                                       static_cast<size_t>(widthPx) * 4);
                            }
                            renderedD2D = true;
                        }
                    }
                }
            }
        }
    }

    if (!renderedD2D) {
        RenderGdiFallback(hdcMem, widthPx, heightPx);
    }

    RECT wndRect{};
    GetWindowRect(m_hwnd, &wndRect);
    POINT dstPt{wndRect.left, wndRect.top};
    SIZE dstSize{widthPx, heightPx};
    POINT srcPt{0, 0};
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = m_alpha;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(m_hwnd, hdcScreen, &dstPt, &dstSize, hdcMem, &srcPt, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

void PurgeToastWindow::RenderGdiFallback(HDC hdcMem, int widthPx, int heightPx) {
    RECT rc{0, 0, widthPx, heightPx};
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdcMem, &rc, bg);
    DeleteObject(bg);

    RECT bar{0, heightPx - ScalePx(4, m_dpi), widthPx, heightPx};
    HBRUSH accent = CreateSolidBrush(RGB(109, 40, 217));
    FillRect(hdcMem, &bar, accent);
    DeleteObject(accent);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(17, 17, 19));
    const wchar_t* text = (m_phase == ToastPhase::Running)
        ? L"VRam - Purging system memory..."
        : L"VRam - Memory optimized";
    RECT txtRc{ScalePx(18, m_dpi), ScalePx(12, m_dpi), widthPx - ScalePx(18, m_dpi), heightPx - ScalePx(14, m_dpi)};
    DrawTextW(hdcMem, text, -1, &txtRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();

    DIBSECTION ds{};
    if (GetObjectW(GetCurrentObject(hdcMem, OBJ_BITMAP), sizeof(ds), &ds) && ds.dsBm.bmBits) {
        auto* p = static_cast<uint8_t*>(ds.dsBm.bmBits);
        const size_t total = static_cast<size_t>(widthPx) * heightPx;
        for (size_t i = 0; i < total; ++i) {
            p[i * 4 + 3] = 255;
        }
    }
}

LRESULT CALLBACK PurgeToastWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PurgeToastWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PurgeToastWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<PurgeToastWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT PurgeToastWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == kTimerAnimation) {
            if (m_phase == ToastPhase::Running) {
                m_spinnerDeg += 14.0f;
                if (m_spinnerDeg >= 360.0f) m_spinnerDeg -= 360.0f;
                m_barPhase += 0.028f;
                if (m_barPhase >= 1.0f) m_barPhase -= 1.0f;
                RenderLayered();
            } else if (m_phase == ToastPhase::Completed) {
                if (!m_hovering) {
                    const uint64_t elapsed = GetTickCount64() - m_completedTickMs;
                    if (elapsed >= m_remainingAutoCloseMs) {
                        m_phase = ToastPhase::FadingOut;
                    }
                }
                RenderLayered();
            } else if (m_phase == ToastPhase::FadingOut) {
                if (m_alpha <= 26) {
                    HideImmediate();
                } else {
                    m_alpha = static_cast<BYTE>(m_alpha - 26);
                    RenderLayered();
                }
            }
            return 0;
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (!m_hovering) {
            m_hovering = true;
            if (m_phase == ToastPhase::Completed) {
                const uint64_t elapsed = GetTickCount64() - m_completedTickMs;
                m_remainingAutoCloseMs = (elapsed < m_remainingAutoCloseMs)
                    ? static_cast<uint32_t>(m_remainingAutoCloseMs - elapsed)
                    : 800u;
            } else if (m_phase == ToastPhase::FadingOut) {
                m_phase = ToastPhase::Completed;
                m_alpha = 255;
                m_remainingAutoCloseMs = 1400u;
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
            TrackMouseEvent(&tme);
        }
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const bool overX = PtInRect(&m_rectCloseX, pt) != FALSE;
        if (overX != m_hoverCloseX) {
            m_hoverCloseX = overX;
            RenderLayered();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_hovering = false;
        m_hoverCloseX = false;
        m_completedTickMs = GetTickCount64();
        if (m_phase == ToastPhase::Completed && m_remainingAutoCloseMs < 1200u) {
            m_remainingAutoCloseMs = 1200u;
        }
        RenderLayered();
        return 0;

    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (PtInRect(&m_rectCloseX, pt)) {
            HideImmediate();
        } else {
            HideImmediate();
            if (m_ownerNotifyHwnd && m_openAppMsg) {
                PostMessageW(m_ownerNotifyHwnd, m_openAppMsg, 0, 0);
            }
        }
        return 0;
    }

    case WM_DPICHANGED:
        m_dpi = HIWORD(wParam);
        PositionBottomRight();
        RenderLayered();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    default:
        return DefWindowProcW(m_hwnd, msg, wParam, lParam);
    }
}

}
