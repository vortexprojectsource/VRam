#include "VectorIcons.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace vram {

namespace {

class MiniSvgPathParser {
public:
    explicit MiniSvgPathParser(const char* src) : m_p(src ? src : "") {}

    bool Build(ID2D1Factory* factory, ComPtr<ID2D1PathGeometry>& outGeo) {
        if (!factory) return false;
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(&geo)) || !geo ||
            FAILED(geo->Open(&sink)) || !sink) {
            return false;
        }
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);

        D2D1_POINT_2F cur{0.0f, 0.0f};
        D2D1_POINT_2F figStart{0.0f, 0.0f};
        D2D1_POINT_2F lastCtrl{0.0f, 0.0f};
        char prevCmd = 0;
        char cmd = 0;
        bool inFigure = false;

        auto closeIfOpen = [&](D2D1_FIGURE_END endType) {
            if (inFigure) {
                sink->EndFigure(endType);
                inFigure = false;
            }
        };

        auto ensureOpenAt = [&](D2D1_POINT_2F pt) {
            if (!inFigure) {
                sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
                inFigure = true;
                figStart = pt;
            }
        };

        while (true) {
            SkipWsComma();
            if (!*m_p) break;

            if (std::isalpha(static_cast<unsigned char>(*m_p))) {
                cmd = *m_p++;
            } else if (cmd == 0) {
                break;
            } else if (cmd == 'M') {
                cmd = 'L';
            } else if (cmd == 'm') {
                cmd = 'l';
            }

            const bool rel = (std::islower(static_cast<unsigned char>(cmd)) != 0);
            const char uc = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

            if (uc == 'Z') {
                closeIfOpen(D2D1_FIGURE_END_CLOSED);
                cur = figStart;
                lastCtrl = cur;
                prevCmd = 'Z';
                cmd = 0;
                continue;
            }

            if (uc == 'M') {
                float x = 0.0f, y = 0.0f;
                if (!ReadFloat(x) || !ReadFloat(y)) break;
                closeIfOpen(D2D1_FIGURE_END_OPEN);
                D2D1_POINT_2F pt = rel ? D2D1_POINT_2F{cur.x + x, cur.y + y} : D2D1_POINT_2F{x, y};
                sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_FILLED);
                inFigure = true;
                figStart = pt;
                cur = pt;
                lastCtrl = cur;
                prevCmd = cmd;
                continue;
            }

            if (uc == 'L') {
                float x = 0.0f, y = 0.0f;
                if (!ReadFloat(x) || !ReadFloat(y)) break;
                ensureOpenAt(cur);
                D2D1_POINT_2F pt = rel ? D2D1_POINT_2F{cur.x + x, cur.y + y} : D2D1_POINT_2F{x, y};
                sink->AddLine(pt);
                cur = pt;
                lastCtrl = cur;
                prevCmd = cmd;
                continue;
            }

            if (uc == 'H') {
                float x = 0.0f;
                if (!ReadFloat(x)) break;
                ensureOpenAt(cur);
                D2D1_POINT_2F pt{rel ? (cur.x + x) : x, cur.y};
                sink->AddLine(pt);
                cur = pt;
                lastCtrl = cur;
                prevCmd = cmd;
                continue;
            }

            if (uc == 'V') {
                float y = 0.0f;
                if (!ReadFloat(y)) break;
                ensureOpenAt(cur);
                D2D1_POINT_2F pt{cur.x, rel ? (cur.y + y) : y};
                sink->AddLine(pt);
                cur = pt;
                lastCtrl = cur;
                prevCmd = cmd;
                continue;
            }

            if (uc == 'C') {
                float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f, x = 0.0f, y = 0.0f;
                if (!ReadFloat(x1) || !ReadFloat(y1) || !ReadFloat(x2) || !ReadFloat(y2) ||
                    !ReadFloat(x) || !ReadFloat(y)) {
                    break;
                }
                ensureOpenAt(cur);
                D2D1_POINT_2F p1 = rel ? D2D1_POINT_2F{cur.x + x1, cur.y + y1} : D2D1_POINT_2F{x1, y1};
                D2D1_POINT_2F p2 = rel ? D2D1_POINT_2F{cur.x + x2, cur.y + y2} : D2D1_POINT_2F{x2, y2};
                D2D1_POINT_2F p3 = rel ? D2D1_POINT_2F{cur.x + x, cur.y + y} : D2D1_POINT_2F{x, y};
                sink->AddBezier(D2D1::BezierSegment(p1, p2, p3));
                lastCtrl = p2;
                cur = p3;
                prevCmd = cmd;
                continue;
            }

            if (uc == 'S') {
                float x2 = 0.0f, y2 = 0.0f, x = 0.0f, y = 0.0f;
                if (!ReadFloat(x2) || !ReadFloat(y2) || !ReadFloat(x) || !ReadFloat(y)) break;
                ensureOpenAt(cur);
                const char prevUpper = static_cast<char>(std::toupper(static_cast<unsigned char>(prevCmd)));
                D2D1_POINT_2F p1 = (prevUpper == 'C' || prevUpper == 'S')
                    ? D2D1_POINT_2F{2.0f * cur.x - lastCtrl.x, 2.0f * cur.y - lastCtrl.y}
                    : cur;
                D2D1_POINT_2F p2 = rel ? D2D1_POINT_2F{cur.x + x2, cur.y + y2} : D2D1_POINT_2F{x2, y2};
                D2D1_POINT_2F p3 = rel ? D2D1_POINT_2F{cur.x + x, cur.y + y} : D2D1_POINT_2F{x, y};
                sink->AddBezier(D2D1::BezierSegment(p1, p2, p3));
                lastCtrl = p2;
                cur = p3;
                prevCmd = cmd;
                continue;
            }

            if (uc == 'A') {
                float rx = 0.0f, ry = 0.0f, xRot = 0.0f;
                int largeArc = 0, sweep = 0;
                float x = 0.0f, y = 0.0f;
                if (!ReadFloat(rx) || !ReadFloat(ry) || !ReadFloat(xRot) ||
                    !ReadFlag(largeArc) || !ReadFlag(sweep) ||
                    !ReadFloat(x) || !ReadFloat(y)) {
                    break;
                }
                ensureOpenAt(cur);
                D2D1_POINT_2F pt = rel ? D2D1_POINT_2F{cur.x + x, cur.y + y} : D2D1_POINT_2F{x, y};
                sink->AddArc(D2D1::ArcSegment(
                    pt,
                    D2D1::SizeF(std::fabs(rx), std::fabs(ry)),
                    xRot,
                    sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    largeArc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                cur = pt;
                lastCtrl = cur;
                prevCmd = cmd;
                continue;
            }

            break;
        }

        closeIfOpen(D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) return false;
        outGeo = geo;
        return true;
    }

private:
    void SkipWsComma() {
        while (*m_p && (std::isspace(static_cast<unsigned char>(*m_p)) || *m_p == ',')) {
            ++m_p;
        }
    }

    bool ReadFloat(float& out) {
        SkipWsComma();
        if (!*m_p) return false;
        char* endPtr = nullptr;
        const float val = std::strtof(m_p, &endPtr);
        if (endPtr == m_p) return false;
        m_p = endPtr;
        out = val;
        return true;
    }

    bool ReadFlag(int& out) {
        SkipWsComma();
        if (*m_p == '0' || *m_p == '1') {
            out = (*m_p - '0');
            ++m_p;
            return true;
        }
        return false;
    }

    const char* m_p;
};

ComPtr<ID2D1Geometry> CombineGeometries(ID2D1Factory* factory, const std::vector<ComPtr<ID2D1Geometry>>& list) {
    if (!factory || list.empty()) return nullptr;
    if (list.size() == 1) return list[0];
    std::vector<ID2D1Geometry*> raw;
    raw.reserve(list.size());
    for (const auto& g : list) {
        if (g) raw.push_back(g.Get());
    }
    if (raw.empty()) return nullptr;
    ComPtr<ID2D1GeometryGroup> group;
    if (FAILED(factory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(),
                                            static_cast<UINT32>(raw.size()), &group))) {
        return nullptr;
    }
    return group;
}

ComPtr<ID2D1Geometry> MakeRoundRect(ID2D1Factory* factory, float l, float t, float w, float h, float r) {
    ComPtr<ID2D1RoundedRectangleGeometry> rr;
    factory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(l, t, l + w, t + h), r, r), &rr);
    return rr;
}

ComPtr<ID2D1Geometry> MakeEllipse(ID2D1Factory* factory, float cx, float cy, float rx, float ry) {
    ComPtr<ID2D1EllipseGeometry> el;
    factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), &el);
    return el;
}

ComPtr<ID2D1Geometry> MakePath(ID2D1Factory* factory, const char* svgPath) {
    ComPtr<ID2D1PathGeometry> pg;
    MiniSvgPathParser parser(svgPath);
    if (parser.Build(factory, pg)) return pg;
    return nullptr;
}

}

bool VectorIconCache::Initialize(ID2D1Factory* factory) {
    if (m_initialized) return true;
    if (!factory) return false;

    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND);
    factory->CreateStrokeStyle(&sp, nullptr, 0, &m_roundStroke);

    auto setStroke = [&](VectorIconId id, const char* path) {
        m_icons[static_cast<size_t>(id)].strokeGeo = MakePath(factory, path);
    };
    auto setFill = [&](VectorIconId id, const char* path) {
        m_icons[static_cast<size_t>(id)].fillGeo = MakePath(factory, path);
    };

    setStroke(VectorIconId::Minimize,     "M5 12h14");
    setStroke(VectorIconId::Close,        "M6 6l12 12 M18 6l-12 12");
    setStroke(VectorIconId::ArrowRight,   "M5 12h14 M13 6l6 6-6 6");
    setStroke(VectorIconId::ChevronRight, "M9 18l6-6-6-6");
    setStroke(VectorIconId::ChevronLeft,  "M15 18l-6-6 6-6");
    setStroke(VectorIconId::Back,         "M15 18L9 12L15 6");
    setStroke(VectorIconId::FolderPlus,   "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7z M12 11v5 M9.5 13.5h5");
    setStroke(VectorIconId::Refresh,      "M21 12a9 9 0 1 1-2.64-6.36 M21 3v6h-6");
    setStroke(VectorIconId::Power,        "M12 3v9 M18.36 6.64a9 9 0 1 1-12.73 0");
    setStroke(VectorIconId::MinimizeToTray, "M4 16v2a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2 M12 4v10 M8 10l4 4 4-4");
    setStroke(VectorIconId::Pin,
              "M12 17v5 M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z");
    setStroke(VectorIconId::SystemCache,  "M12 2L2 7l10 5 10-5-10-5z M2 12l10 5 10-5 M2 17l10 5 10-5");
    setStroke(VectorIconId::ModifiedList, "M21 12a9 9 0 0 0-9-9 9.75 9.75 0 0 0-6.74 2.74L3 8 M3 3v5h5 M3 12a9 9 0 0 0 9 9 9.75 9.75 0 0 0 6.74-2.74L21 16 M16 16h5v5");
    setStroke(VectorIconId::Gauge,        "M12 14l4-4 M3.34 19a10 10 0 1 1 17.32 0");
    setStroke(VectorIconId::Bell,         "M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9 M13.73 21a2 2 0 0 1-3.46 0");
    setStroke(VectorIconId::Shield,       "M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z");

    setFill(VectorIconId::Lightning, "M13 2L4.5 13.5H11.5L10 22L19.5 10.5H12.5L13 2Z");
    setFill(VectorIconId::Heart,
            "M12 21s-7-4.35-9.33-8.5C.67 8.92 2.92 4.5 7 4.5c2.04 0 3.74 1.07 5 2.7C13.26 5.57 14.96 4.5 17 4.5c4.08 0 6.33 4.42 4.33 8C19 16.65 12 21 12 21z");

    // Composite icons (rounded boxes / ellipses + strokes)
    m_icons[static_cast<size_t>(VectorIconId::Gamepad)].strokeGeo = CombineGeometries(factory, {
        MakeRoundRect(factory, 2.0f, 6.0f, 20.0f, 12.0f, 4.0f),
        MakePath(factory, "M6 12h4 M8 10v4")
    });
    m_icons[static_cast<size_t>(VectorIconId::Gamepad)].fillGeo = CombineGeometries(factory, {
        MakeEllipse(factory, 14.9f, 13.1f, 1.15f, 1.15f),
        MakeEllipse(factory, 17.3f, 10.9f, 1.15f, 1.15f)
    });

    m_icons[static_cast<size_t>(VectorIconId::Settings)].strokeGeo = CombineGeometries(factory, {
        MakeEllipse(factory, 12.0f, 12.0f, 3.0f, 3.0f),
        MakePath(factory,
                 "M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 "
                 "1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09a1.65 1.65 0 0 0-1-1.51 1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 "
                 "1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09a1.65 1.65 0 0 0 1.51-1 1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 "
                 "2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 "
                 "1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9c.26.6.77 1 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z")
    });

    m_icons[static_cast<size_t>(VectorIconId::HideTray)].strokeGeo = CombineGeometries(factory, {
        MakeRoundRect(factory, 3.0f, 4.0f, 18.0f, 16.0f, 2.0f),
        MakePath(factory, "M3 15h18 M10 9.5l2 2 2-2")
    });

    m_icons[static_cast<size_t>(VectorIconId::StartMinimized)].strokeGeo = CombineGeometries(factory, {
        MakeRoundRect(factory, 3.0f, 4.0f, 18.0f, 16.0f, 2.0f),
        MakePath(factory, "M3 15h18 M10 10l2 2 2-2")
    });

    m_icons[static_cast<size_t>(VectorIconId::CloseToTray)].strokeGeo = CombineGeometries(factory, {
        MakeRoundRect(factory, 3.0f, 4.0f, 18.0f, 16.0f, 2.0f),
        MakePath(factory, "M9 9l6 6 M15 9l-6 6")
    });

    m_icons[static_cast<size_t>(VectorIconId::WorkingSets)].strokeGeo = CombineGeometries(factory, {
        MakeRoundRect(factory, 4.0f, 6.0f, 16.0f, 10.0f, 2.0f),
        MakePath(factory, "M8 16v3M12 16v3M16 16v3M8 6V3M12 6V3M16 6V3")
    });

    m_icons[static_cast<size_t>(VectorIconId::StandbyList)].strokeGeo = CombineGeometries(factory, {
        MakeEllipse(factory, 12.0f, 5.0f, 9.0f, 3.0f),
        MakePath(factory, "M21 12c0 1.66-4 3-9 3s-9-1.34-9-3 M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5")
    });

    m_icons[static_cast<size_t>(VectorIconId::Clock)].strokeGeo = CombineGeometries(factory, {
        MakeEllipse(factory, 12.0f, 12.0f, 9.0f, 9.0f),
        MakePath(factory, "M12 7v5l3 2")
    });

    m_initialized = true;
    return true;
}

void VectorIconCache::Draw(ID2D1RenderTarget* rt,
                           VectorIconId id,
                           const D2D1_RECT_F& bounds,
                           ID2D1Brush* brush,
                           float strokeWidthDip,
                           float rotationDeg) const {
    if (!rt || !brush) return;
    const size_t idx = static_cast<size_t>(id);
    if (idx >= m_icons.size()) return;

    const float bw = bounds.right - bounds.left;
    const float bh = bounds.bottom - bounds.top;
    if (bw <= 0.0f || bh <= 0.0f) return;

    const float sx = bw / 24.0f;
    const float sy = bh / 24.0f;

    D2D1_MATRIX_3X2_F oldTx{};
    rt->GetTransform(&oldTx);

    D2D1_MATRIX_3X2_F tx = D2D1::Matrix3x2F::Scale(sx, sy);
    if (rotationDeg != 0.0f) {
        tx = D2D1::Matrix3x2F::Rotation(rotationDeg, D2D1::Point2F(12.0f, 12.0f)) * tx;
    }
    tx = tx * D2D1::Matrix3x2F::Translation(bounds.left, bounds.top) * oldTx;
    rt->SetTransform(tx);

    const float avgScale = std::max(0.05f, (sx + sy) * 0.5f);
    if (m_icons[idx].fillGeo) {
        rt->FillGeometry(m_icons[idx].fillGeo.Get(), brush);
    }
    if (m_icons[idx].strokeGeo) {
        rt->DrawGeometry(m_icons[idx].strokeGeo.Get(), brush, strokeWidthDip / avgScale, m_roundStroke.Get());
    }

    rt->SetTransform(oldTx);
}

}
