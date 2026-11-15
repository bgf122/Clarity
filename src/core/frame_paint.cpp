#include "frame_paint.h"

#include <algorithm>
#include <cmath>

namespace prk::frame {

using gfx::Canvas;

int Px(float v, float scale)
{
    return static_cast<int>(std::lround(v * scale));
}

int Outline(float)
{
    return 1;
}
int Ring(float scale)
{
    return std::max(2, Px(4, scale));
}
int Inner(float scale)
{
    return Outline(scale) + Ring(scale);
}
float Cut(float scale)
{
    return static_cast<float>(std::max(8, Px(16, scale)));
}
float InnerCut(float cut, float d)
{
    return std::max(0.0f, cut - d * 0.5858f);
}
int Bezel(float scale)
{
    return std::max(3, Px(4, scale));
}

void TealPath(Canvas& c, const std::vector<std::pair<float, float>>& pts, float alpha, float scale)
{
    const float glow = static_cast<float>(std::max(2, Px(3, scale)));
    const float core = static_cast<float>(std::max(1, Px(1, scale)));
    for (size_t i = 1; i < pts.size(); ++i)
        c.Line(pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second, core + 2.0f * glow, kTealRgb, 0.10f * alpha);
    for (size_t i = 1; i < pts.size(); ++i)
        c.Line(pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second, core, Canvas::Mix(kTealRgb, 0xFFFFFF, 0.25f),
               0.85f * alpha);
}

void PaintFrame(Canvas& c, float scale)
{
    const float W = static_cast<float>(c.w), H = static_cast<float>(c.h), cut = Cut(scale);
    const float ring = static_cast<float>(Ring(scale));
    const float r0 = static_cast<float>(Outline(scale)), r1 = r0 + ring; // the ring, outside and inside
    auto flat = [](uint32_t rgb) { return [rgb](float, int) { return rgb; }; };
    auto above = [&](float y) { return [y](float, float py) { return py < y; }; };

    // The outline, near black, then the gunmetal ring with a light line along its top.
    c.Chamfer(0.0f, 0.0f, W, H, cut, 1.0f, flat(0x0B0C0E));
    c.Chamfer(r0, r0, W - r0, H - r0, InnerCut(cut, r0), 1.0f, [](float t, int) { return Canvas::Mix(0x41454A, 0x2B2E32, t); });
    c.ChamferStroke(r0, r0, W - r0, H - r0, InnerCut(cut, r0), 1.0f, 0x6A6F74, 0.8f, above(H * 0.5f));
    // The inner panel, and steps in the ring: thicker along the middle of the top and of the bottom,
    // with 45-degree ends.
    c.Chamfer(r1, r1, W - r1, H - r1, InnerCut(cut, r1), 1.0f, [](float t, int) { return Canvas::Mix(0x191B1E, 0x101214, t); });
    const float step = TopStepDepth(scale);
    const uint32_t ringRgb = 0x34383C;
    PaintTopStep(c, scale);
    c.Polygon({{W * 0.34f + step, H - r1 - step}, {W * 0.66f - step, H - r1 - step}, {W * 0.66f, H - r1 + 0.5f}, {W * 0.34f, H - r1 + 0.5f}},
              ringRgb, 1.0f);

    // Teal along the inner panel's lower corners: up the side, round the corner, along the bottom to the
    // step and up it; less of it on the right.
    const float t = r1 + 1.5f, ic = InnerCut(cut, t);
    TealPath(c, {{t, H * 0.5f}, {t, H - t - ic}, {t + ic, H - t}, {W * 0.34f, H - t}, {W * 0.34f + step, H - t - step}, {W * 0.42f, H - t - step}},
             1.0f, scale);
    TealPath(c, {{W - t, H * 0.62f}, {W - t, H - t - ic}, {W - t - ic, H - t}, {W * 0.8f, H - t}}, 0.7f, scale);
    // A teal light on the top right, in the ring, with a slanted mark before it.
    const float ly = r0 + ring * 0.5f, lx1 = W - cut - Px(14, scale), lx0 = lx1 - Px(22, scale);
    TealPath(c, {{lx0, ly}, {lx1, ly}}, 1.0f, scale);
    c.Line(lx0 - Px(10, scale), ly + ring * 0.5f, lx0 - Px(6, scale), ly - ring * 0.5f, 1.0f, 0x6A6F74, 0.8f);
}

float TopStepDepth(float scale)
{
    return static_cast<float>(std::max(2, Px(4, scale)));
}

void PaintTopStep(Canvas& c, float scale)
{
    const float W = static_cast<float>(c.w);
    const float r1 = static_cast<float>(Outline(scale) + Ring(scale));
    const float step = TopStepDepth(scale);
    c.Polygon({{W * 0.27f, r1 - 0.5f}, {W * 0.73f, r1 - 0.5f}, {W * 0.73f - step, r1 + step}, {W * 0.27f + step, r1 + step}}, 0x34383C, 1.0f);
}

float InnerDistance(float W, float H, float scale, float px, float py)
{
    const float r1 = static_cast<float>(Outline(scale) + Ring(scale));
    return Canvas::ChamferDist(px, py, r1, r1, W - r1, H - r1, InnerCut(Cut(scale), r1));
}

std::vector<uint32_t> Gloss(uint32_t rgb)
{
    std::vector<uint32_t> rows(24);
    for (size_t i = 0; i < rows.size(); ++i) {
        const float t = (i + 0.5f) / rows.size();
        if (t < 0.08f)
            rows[i] = Canvas::Mix(rgb, 0xFFFFFF, 0.55f);
        else if (t < 0.45f)
            rows[i] = Canvas::Mix(rgb, 0xFFFFFF, 0.28f * (0.45f - t) / 0.37f);
        else if (t < 0.85f)
            rows[i] = gfx::Darker(rgb, 1.0f - (t - 0.45f) * 0.5f);
        else if (t < 0.94f)
            rows[i] = gfx::Darker(rgb, 0.78f);
        else
            rows[i] = Canvas::Mix(rgb, 0xFFFFFF, 0.3f);
    }
    return rows;
}

void BarBezel(Canvas& c, int x, int y, int w, int h, float scale)
{
    const float b = static_cast<float>(Bezel(scale));
    const float fx = static_cast<float>(x), fy = static_cast<float>(y), fw = static_cast<float>(w), fh = static_cast<float>(h);
    c.RoundRect(fx - b - 1.0f, fy - b - 1.0f, fx + fw + b + 1.0f, fy + fh + b + 1.0f, b + 2.0f, 0x0A0B0D, 0x0A0B0D, 1.0f);
    c.RoundRect(fx - b, fy - b, fx + fw + b, fy + fh + b, b + 1.0f, 0x8A8F94, 0x4B4F53, 1.0f);
    c.RoundRect(fx - b + 1.0f, fy - b + 1.0f, fx + fw + b - 1.0f, fy + fh + b - 1.0f, b, 0x3A3E42, 0x2C2F33, 1.0f);
    c.RoundRect(fx - 1.0f, fy - 1.0f, fx + fw + 1.0f, fy + fh + 1.0f, 1.5f, 0x040506, 0x040506, 1.0f);
}

Canvas BarBody(int w, int h, float fraction, uint32_t rgb, float section, const std::vector<uint32_t>* shading)
{
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    Canvas bar;
    bar.Reset(w, h);
    bar.RoundRect(0.0f, 0.0f, fw, fh, 0.0f, 0x0B0D10, 0x17191D, 1.0f);
    if (fraction >= 0.0f) {
        const float fill = fw * std::min(fraction, 1.0f);
        const std::vector<uint32_t> gloss = shading && !shading->empty() ? *shading : Gloss(rgb);
        bar.RoundRect(0.0f, 0.0f, fw, fh, 0.0f, rgb, rgb, 1.0f, 0.0f, fill, &gloss);
        const uint32_t edge = Canvas::Mix(rgb, 0xFFFFFF, 0.45f); // its bright end
        if (fill >= 2.0f)
            for (int ly = 0; ly < h; ++ly)
                bar.Blend(static_cast<int>(fill) - 1, ly, edge, 0.7f);
    }
    // Section lines, lit on their right where the bar is filled.
    for (int i = 1; section >= 2.0f && i * section < fw - section * 0.5f; ++i) {
        const int sx = static_cast<int>(std::lround(i * section));
        for (int sy = 0; sy < h; ++sy) {
            bar.Blend(sx - 1, sy, 0x000000, 0.5f);
            if (fraction >= 0.0f && sx < fw * fraction)
                bar.Blend(sx, sy, 0xFFFFFF, 0.18f);
        }
    }
    return bar;
}

} // namespace prk::frame
