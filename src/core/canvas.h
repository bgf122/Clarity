#pragma once

// A premultiplied float RGBA image Clarity paints its frames, bars and labels into (in software, with
// antialiasing), then converts and uploads to a GUI surface. Shared by the frames (bars) and the
// action bars, and by the preview tool, so it has no game dependencies.
#include "text_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace prk::gfx {

// Premultiplied float RGBA.
struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;

    void Reset(int width, int height)
    {
        w = width;
        h = height;
        px.assign(static_cast<size_t>(w) * h * 4, 0.0f);
    }

    void Blend(int x, int y, uint32_t rgb, float a)
    {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f)
            return;
        a = std::min(a, 1.0f);
        float* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        p[0] = ((rgb >> 16) & 0xFF) / 255.0f * a + p[0] * (1.0f - a);
        p[1] = ((rgb >> 8) & 0xFF) / 255.0f * a + p[1] * (1.0f - a);
        p[2] = (rgb & 0xFF) / 255.0f * a + p[2] * (1.0f - a);
        p[3] = a + p[3] * (1.0f - a);
    }

    // An antialiased rounded rectangle from (x0, y0) to (x1, y1) (exclusive, fractional), its colour
    // going from rgbTop to rgbBottom, cut off left of cx0 and right of cx1 (the filled part of a bar).
    void RoundRect(float x0, float y0, float x1, float y1, float radius, uint32_t rgbTop, uint32_t rgbBottom, float alpha,
                   float cx0 = -1e9f, float cx1 = 1e9f, const std::vector<uint32_t>* profile = nullptr)
    {
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
        if (hx <= 0.0f || hy <= 0.0f || cx1 <= cx0)
            return;
        const float mx = x0 + hx, my = y0 + hy;
        const float r = std::min(radius, std::min(hx, hy));
        for (int y = static_cast<int>(std::floor(y0)); y < static_cast<int>(std::ceil(y1)); ++y) {
            const float cy = y + 0.5f;
            const float t = std::clamp((cy - y0) / (y1 - y0), 0.0f, 1.0f);
            const uint32_t rgb = profile && !profile->empty()
                                     ? ProfileAt(*profile, (y - y0) / (y1 - y0), (y + 1 - y0) / (y1 - y0))
                                     : Mix(rgbTop, rgbBottom, t);
            for (int x = static_cast<int>(std::floor(std::max(x0, cx0))); x < static_cast<int>(std::ceil(std::min(x1, cx1))); ++x) {
                const float cx = x + 0.5f;
                const float qx = std::fabs(cx - mx) - (hx - r), qy = std::fabs(cy - my) - (hy - r);
                const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
                const float cover = std::clamp(0.5f - d, 0.0f, 1.0f);
                const float horiz = std::clamp(std::min(x + 1.0f, cx1) - std::max(static_cast<float>(x), cx0), 0.0f, 1.0f);
                Blend(x, y, rgb, alpha * cover * horiz);
            }
        }
    }

    // Signed distance (pixels, < 0 inside) from a point to the rectangle (x0, y0)-(x1, y1) with its
    // corners cut off at 45 degrees, `cut` along each side.
    static float ChamferDist(float px, float py, float x0, float y0, float x1, float y1, float cut)
    {
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
        const float ax = std::fabs(px - (x0 + hx)), ay = std::fabs(py - (y0 + hy));
        return std::max(std::max(ax - hx, ay - hy), (ax + ay - (hx + hy - cut)) * 0.70710678f);
    }

    // The cut-corner rectangle filled, antialiased, in rgbAt(t, row) (t from 0 at the top to 1 at
    // the bottom).
    template <typename F>
    void Chamfer(float x0, float y0, float x1, float y1, float cut, float alpha, F&& rgbAt)
    {
        for (int y = std::max(0, static_cast<int>(std::floor(y0))); y < std::min(h, static_cast<int>(std::ceil(y1))); ++y) {
            const uint32_t rgb = rgbAt(std::clamp((y + 0.5f - y0) / (y1 - y0), 0.0f, 1.0f), y);
            for (int x = std::max(0, static_cast<int>(std::floor(x0))); x < std::min(w, static_cast<int>(std::ceil(x1))); ++x) {
                const float d = ChamferDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, cut);
                Blend(x, y, rgb, alpha * std::clamp(0.5f - d, 0.0f, 1.0f));
            }
        }
    }

    // A line `width` pixels wide just inside the cut-corner rectangle's edge, where keep(x, y) says.
    template <typename K>
    void ChamferStroke(float x0, float y0, float x1, float y1, float cut, float width, uint32_t rgb, float alpha, K&& keep)
    {
        for (int y = std::max(0, static_cast<int>(std::floor(y0))); y < std::min(h, static_cast<int>(std::ceil(y1))); ++y) {
            for (int x = std::max(0, static_cast<int>(std::floor(x0))); x < std::min(w, static_cast<int>(std::ceil(x1))); ++x) {
                if (!keep(x + 0.5f, y + 0.5f))
                    continue;
                const float d = ChamferDist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, cut);
                const float cover = std::clamp(0.5f - d, 0.0f, 1.0f) - std::clamp(0.5f - (d + width), 0.0f, 1.0f);
                Blend(x, y, rgb, alpha * cover);
            }
        }
    }

    // A convex polygon filled, antialiased.
    void Polygon(const std::vector<std::pair<float, float>>& pts, uint32_t rgb, float alpha)
    {
        float area = 0.0f, minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (size_t i = 0; i < pts.size(); ++i) {
            const auto& a = pts[i];
            const auto& b = pts[(i + 1) % pts.size()];
            area += a.first * b.second - b.first * a.second;
            minX = std::min(minX, a.first), maxX = std::max(maxX, a.first);
            minY = std::min(minY, a.second), maxY = std::max(maxY, a.second);
        }
        const float sign = area > 0.0f ? 1.0f : -1.0f;
        for (int y = std::max(0, static_cast<int>(std::floor(minY)) - 1); y < std::min(h, static_cast<int>(std::ceil(maxY)) + 1); ++y) {
            for (int x = std::max(0, static_cast<int>(std::floor(minX)) - 1); x < std::min(w, static_cast<int>(std::ceil(maxX)) + 1); ++x) {
                float d = -1e9f;
                for (size_t i = 0; i < pts.size(); ++i) {
                    const auto& a = pts[i];
                    const auto& b = pts[(i + 1) % pts.size()];
                    const float ex = b.first - a.first, ey = b.second - a.second;
                    const float len = std::hypot(ex, ey);
                    if (len <= 0.0f)
                        continue;
                    // Outward normal: right of the edge for a clockwise (on screen) polygon.
                    const float nx = sign * ey / len, ny = -sign * ex / len;
                    d = std::max(d, (x + 0.5f - a.first) * nx + (y + 0.5f - a.second) * ny);
                }
                Blend(x, y, rgb, alpha * std::clamp(0.5f - d, 0.0f, 1.0f));
            }
        }
    }

    // A line `width` pixels thick from (x0, y0) to (x1, y1), antialiased.
    void Line(float x0, float y0, float x1, float y1, float width, uint32_t rgb, float alpha)
    {
        const float r = width * 0.5f;
        const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
        for (int y = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - r)) - 1);
             y < std::min(h, static_cast<int>(std::ceil(std::max(y0, y1) + r)) + 1); ++y) {
            for (int x = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - r)) - 1);
                 x < std::min(w, static_cast<int>(std::ceil(std::max(x0, x1) + r)) + 1); ++x) {
                const float qx = x + 0.5f - x0, qy = y + 0.5f - y0;
                const float t = len2 > 0.0f ? std::clamp((qx * dx + qy * dy) / len2, 0.0f, 1.0f) : 0.0f;
                const float d = std::hypot(qx - t * dx, qy - t * dy);
                Blend(x, y, rgb, alpha * std::clamp(r + 0.5f - d, 0.0f, 1.0f));
            }
        }
    }

    // Another canvas over this one, its top-left at (x0, y0).
    void Paste(const Canvas& o, int x0, int y0)
    {
        for (int y = 0; y < o.h; ++y) {
            for (int x = 0; x < o.w; ++x) {
                const int tx = x0 + x, ty = y0 + y;
                const float* s = &o.px[(static_cast<size_t>(y) * o.w + x) * 4];
                if (s[3] <= 0.0f || tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
                float* d = &px[(static_cast<size_t>(ty) * w + tx) * 4];
                for (int i = 0; i < 4; ++i)
                    d[i] = s[i] + d[i] * (1.0f - s[3]);
            }
        }
    }

    void Mask(int x0, int y0, const raster::Mask& m, uint32_t rgb, float alpha)
    {
        for (int y = 0; y < m.h; ++y)
            for (int x = 0; x < m.w; ++x)
                Blend(x0 + x, y0 + y, rgb, m.a[static_cast<size_t>(y) * m.w + x] * alpha);
    }

    // The average colour of the profile between fractions t0 and t1 of its length.
    static uint32_t ProfileAt(const std::vector<uint32_t>& profile, float t0, float t1)
    {
        const float n = static_cast<float>(profile.size());
        const float a = std::clamp(t0, 0.0f, 1.0f) * n, b = std::max(std::clamp(t1, 0.0f, 1.0f) * n, a + 1e-3f);
        float r = 0, g = 0, bl = 0, wsum = 0;
        for (int i = static_cast<int>(a); i < static_cast<int>(std::ceil(b)) && i < static_cast<int>(n); ++i) {
            const float w = std::min(b, i + 1.0f) - std::max(a, static_cast<float>(i));
            if (w <= 0.0f)
                continue;
            const uint32_t c = profile[static_cast<size_t>(i)];
            r += ((c >> 16) & 0xFF) * w;
            g += ((c >> 8) & 0xFF) * w;
            bl += (c & 0xFF) * w;
            wsum += w;
        }
        if (wsum <= 0.0f)
            return profile.back();
        return (static_cast<uint32_t>(r / wsum + 0.5f) << 16) | (static_cast<uint32_t>(g / wsum + 0.5f) << 8) |
               static_cast<uint32_t>(bl / wsum + 0.5f);
    }

    static uint32_t Mix(uint32_t a, uint32_t b, float t)
    {
        auto ch = [&](int shift) {
            const float va = static_cast<float>((a >> shift) & 0xFF), vb = static_cast<float>((b >> shift) & 0xFF);
            return static_cast<uint32_t>(std::lround(va + (vb - va) * t)) << shift;
        };
        return ch(16) | ch(8) | ch(0);
    }
};

inline uint32_t Darker(uint32_t rgb, float f)
{
    return Canvas::Mix(0x000000, rgb, f);
}

} // namespace prk::gfx
