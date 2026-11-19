#pragma once

#include <d2d1.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>

namespace vram {

enum class VectorIconId : size_t {
    Minimize = 0,
    Close,
    ArrowRight,
    ChevronRight,
    Back,
    Gamepad,
    Settings,
    HideTray,
    FolderPlus,
    Refresh,
    Power,
    StartMinimized,
    MinimizeToTray,
    CloseToTray,
    Lightning,
    Pin,
    WorkingSets,
    SystemCache,
    StandbyList,
    ModifiedList,
    Gauge,
    Clock,
    Bell,
    Shield,
    Heart,
    ChevronLeft,
    Count
};

/// Caches 24x24 Direct2D vector icon geometries parsed directly from SVG path specs.
class VectorIconCache {
public:
    bool Initialize(ID2D1Factory* factory);

    void Draw(ID2D1RenderTarget* rt,
              VectorIconId id,
              const D2D1_RECT_F& bounds,
              ID2D1Brush* brush,
              float strokeWidthDip = 1.8f,
              float rotationDeg = 0.0f) const;

private:
    struct IconGeometries {
        Microsoft::WRL::ComPtr<ID2D1Geometry> strokeGeo;
        Microsoft::WRL::ComPtr<ID2D1Geometry> fillGeo;
    };

    std::array<IconGeometries, static_cast<size_t>(VectorIconId::Count)> m_icons;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> m_roundStroke;
    bool m_initialized = false;
};

}
