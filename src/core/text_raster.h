#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

// Antialiased text masks via GDI (4x supersampled), plus outline and shadow, for building
// textures (floating-text glyph atlas, nameplates).
namespace prk::raster {

constexpr int kSuper = 4;

struct Mask {
    int w = 0, h = 0;
    std::vector<float> a; // coverage 0..1, row-major

    Mask() = default;
    Mask(int width, int height) : w(width), h(height), a(static_cast<size_t>(width) * height, 0.0f) {}
    float At(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0.0f : a[y * w + x]; }
};

// A memory DC with a font created at kSuper x the requested pixel height (em, negative = em).
struct Font {
    HDC dc = nullptr;
    HFONT font = nullptr;
    TEXTMETRICW tm{}; // at kSuper x scale

    bool Create(const wchar_t* face, int emPx, int weight);
    void Destroy();
    float Ascent() const { return static_cast<float>(tm.tmAscent) / kSuper; }
    float Height() const { return static_cast<float>(tm.tmHeight) / kSuper; }
    float TextWidth(const wchar_t* s, int len) const; // pixels
};

// Coverage of `s` drawn with its top-left at (x, y) pixels in a w x h mask.
Mask Rasterize(const Font& font, const wchar_t* s, int len, int x, int y, int w, int h);

// Disc dilation with an antialiased rim (outline).
Mask Dilate(const Mask& c, int radius);

// The mask moved down-right by `offset` and box-blurred `blur` px (drop shadow).
Mask Shadow(const Mask& o, int offset, int blur);

} // namespace prk::raster
