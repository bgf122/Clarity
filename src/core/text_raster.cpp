#include "text_raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace prk::raster {

bool Font::Create(const wchar_t* face, int emPx, int weight)
{
    dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return false;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, TA_TOP | TA_LEFT);
    font = CreateFontW(-emPx * kSuper, 0, 0, 0, std::clamp(weight, 100, 900), FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    if (!font) {
        DeleteDC(dc);
        dc = nullptr;
        return false;
    }
    SelectObject(dc, font);
    GetTextMetricsW(dc, &tm);
    return true;
}

void Font::Destroy()
{
    if (font)
        DeleteObject(font);
    if (dc)
        DeleteDC(dc);
    font = nullptr;
    dc = nullptr;
}

float Font::TextWidth(const wchar_t* s, int len) const
{
    SIZE size{};
    GetTextExtentPoint32W(dc, s, len, &size);
    return static_cast<float>(size.cx) / kSuper;
}

Mask Rasterize(const Font& font, const wchar_t* s, int len, int x, int y, int w, int h)
{
    Mask m(w, h);
    const int W = w * kSuper;
    const int H = h * kSuper;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(font.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits)
        return m;
    HGDIOBJ old = SelectObject(font.dc, bmp);
    memset(bits, 0, static_cast<size_t>(W) * H * 4);
    TextOutW(font.dc, x * kSuper, y * kSuper, s, len);
    GdiFlush();

    const auto* px = static_cast<const uint32_t*>(bits);
    const float norm = 1.0f / (255.0f * kSuper * kSuper);
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            unsigned sum = 0;
            for (int sy = 0; sy < kSuper; ++sy)
                for (int sx = 0; sx < kSuper; ++sx)
                    sum += px[(yy * kSuper + sy) * W + xx * kSuper + sx] & 0xFF;
            m.a[yy * w + xx] = static_cast<float>(sum) * norm;
        }
    }
    SelectObject(font.dc, old);
    DeleteObject(bmp);
    return m;
}

Mask Dilate(const Mask& c, int radius)
{
    Mask o = c;
    if (radius <= 0)
        return o;
    struct Tap {
        int dx, dy;
        float w;
    };
    std::vector<Tap> taps;
    for (int dy = -radius - 1; dy <= radius + 1; ++dy) {
        for (int dx = -radius - 1; dx <= radius + 1; ++dx) {
            const float w = std::clamp(radius + 0.5f - std::sqrt(static_cast<float>(dx * dx + dy * dy)), 0.0f, 1.0f);
            if (w > 0.0f)
                taps.push_back({dx, dy, w});
        }
    }
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            float v = 0.0f;
            for (const Tap& t : taps)
                v = std::max(v, c.At(x + t.dx, y + t.dy) * t.w);
            o.a[y * c.w + x] = v;
        }
    }
    return o;
}

Mask Shadow(const Mask& o, int offset, int blur)
{
    Mask s = o;
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
            s.a[y * o.w + x] = o.At(x - offset, y - offset);
    for (int pass = 0; pass < 2 && blur > 0; ++pass) {
        Mask t = s;
        const float n = 1.0f / (2 * blur + 1);
        for (int y = 0; y < s.h; ++y) {
            for (int x = 0; x < s.w; ++x) {
                float v = 0.0f;
                for (int k = -blur; k <= blur; ++k)
                    v += pass == 0 ? s.At(x + k, y) : s.At(x, y + k);
                t.a[y * s.w + x] = v * n;
            }
        }
        s = t;
    }
    return s;
}

} // namespace prk::raster
