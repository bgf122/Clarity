#include "atlas.h"

#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/log.h"
#include "../../core/text_raster.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace prk::fct::atlas {

namespace {

constexpr int kTexW = 1024;
constexpr int kTexH = 1024;
// DiaBill decodes frame codes against a 128x64 grid (u = x / 128, v = y / 64), so cells must
// sit on multiples of these texel steps.
constexpr int kUnitX = kTexW / 128;
constexpr int kUnitY = kTexH / 64;
constexpr int kCellH = 64; // texels, multiple of kUnitY
constexpr float kShadowAlpha = 0.6f;

Atlas g_atlas;
bool g_tried = false;
bool g_ok = false;

void Blit(std::vector<uint32_t>& tex, int tx, int ty, const raster::Mask& alpha, uint32_t rgb)
{
    for (int y = 0; y < alpha.h; ++y) {
        for (int x = 0; x < alpha.w; ++x) {
            const float a = std::clamp(alpha.a[y * alpha.w + x], 0.0f, 1.0f);
            tex[static_cast<size_t>(ty + y) * kTexW + tx + x] =
                (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | (rgb & 0xFFFFFF);
        }
    }
}

bool Build(void* gameMaterial)
{
    const FctConfig& cfg = Cfg().fct;

    // Outline/shadow sizes are given in screen pixels at SizePx; convert to texels using an
    // estimate of the cap height (~0.7 em) - the atlas is built once.
    raster::Font font;
    int pad = 0, radius = 0, shadowOff = 0, blur = 0;
    for (int em = 48; em >= 16; em -= 2) {
        const float texelsPerPx = 0.7f * em / std::max(cfg.sizePx, 6.0f);
        radius = cfg.outline ? std::clamp(static_cast<int>(std::lround(cfg.outlinePx * texelsPerPx)), 1, 8) : 0;
        shadowOff = cfg.shadowPx > 0.0f ? std::clamp(static_cast<int>(std::lround(cfg.shadowPx * texelsPerPx)), 1, 6) : 0;
        blur = shadowOff > 0 ? 2 : 0;
        pad = radius + shadowOff + blur + 1;
        font.Destroy();
        if (!font.Create(cfg.fontName, em, cfg.fontWeight))
            continue;
        if (font.Height() + 2 * pad <= kCellH)
            break;
    }
    if (!font.dc)
        return false;

    std::vector<uint32_t> tex(static_cast<size_t>(kTexW) * kTexH, 0);
    const int top = pad;
    g_atlas.cellH = static_cast<float>(kCellH);
    g_atlas.baseline = static_cast<float>(top) + font.Ascent();

    ABC space{};
    GetCharABCWidthsW(font.dc, L' ', L' ', &space);
    g_atlas.spaceAdvance = static_cast<float>(space.abcA + static_cast<int>(space.abcB) + space.abcC) / raster::kSuper;

    int ux = 0, uy = 0; // packing cursor in frame units
    const int hUnits = kCellH / kUnitY;
    bool fits = true;
    for (int c = 33; c < 127 && fits; ++c) {
        ABC abc{};
        if (!GetCharABCWidthsW(font.dc, static_cast<UINT>(c), static_cast<UINT>(c), &abc))
            continue;
        const float a = static_cast<float>(abc.abcA) / raster::kSuper;
        const float b = static_cast<float>(abc.abcB) / raster::kSuper;
        const float advance = static_cast<float>(abc.abcA + static_cast<int>(abc.abcB) + abc.abcC) / raster::kSuper;
        const int originX = pad + static_cast<int>(std::ceil(std::max(0.0f, -a)));
        const float right = std::max(originX + a + b, originX + advance) + pad;
        const int wUnits = static_cast<int>(std::ceil(right / kUnitX));
        const int cellW = wUnits * kUnitX;

        const wchar_t ch = static_cast<wchar_t>(c);
        const raster::Mask fill = raster::Rasterize(font, &ch, 1, originX, top, cellW, kCellH);
        const raster::Mask outline = raster::Dilate(fill, radius);
        raster::Mask edge = outline;
        if (shadowOff > 0) {
            const raster::Mask shadow = raster::Shadow(outline, shadowOff, blur);
            for (size_t i = 0; i < edge.a.size(); ++i)
                edge.a[i] = std::max(outline.a[i], kShadowAlpha * shadow.a[i]);
        }

        Glyph& g = g_atlas.glyphs[c];
        for (int variant = 0; variant < 2; ++variant) {
            if (ux + wUnits > 128) {
                ux = 0;
                uy += hUnits;
            }
            if ((uy + hUnits) * kUnitY > kTexH) {
                fits = false;
                break;
            }
            const uint32_t frame = (static_cast<uint32_t>(ux) << 24) | (static_cast<uint32_t>(uy) << 16) |
                                   (static_cast<uint32_t>(wUnits) << 8) | static_cast<uint32_t>(hUnits);
            if (variant == 0) {
                Blit(tex, ux * kUnitX, uy * kUnitY, edge, 0x000000);
                g.outlineFrame = frame;
            } else {
                Blit(tex, ux * kUnitX, uy * kUnitY, fill, 0xFFFFFF);
                g.fillFrame = frame;
            }
            ux += wUnits;
        }
        g.originX = static_cast<float>(originX);
        g.cellW = static_cast<float>(cellW);
        g.advance = advance;
        g.present = fits;

        // Cap height = ink height of '0' (what SizePx refers to).
        if (c == '0') {
            int first = -1, last = -1;
            for (int y = 0; y < kCellH; ++y) {
                for (int x = 0; x < cellW; ++x) {
                    if (fill.a[y * cellW + x] > 0.5f) {
                        if (first < 0)
                            first = y;
                        last = y;
                        break;
                    }
                }
            }
            g_atlas.capH = first >= 0 ? static_cast<float>(last - first + 1) : font.Ascent();
        }
    }

    wchar_t face[64] = {};
    GetTextFaceW(font.dc, 64, face);
    font.Destroy();
    if (!fits || g_atlas.capH <= 0.0f) {
        Log("[fct] glyph atlas: doesn't fit / no cap height - using the game font");
        return false;
    }

    void* texture = game::CreateTextureARGB("prkui_fct_glyphs", kTexW, kTexH);
    if (!texture || !game::WriteTexture(texture, tex.data(), kTexW, kTexH)) {
        Log("[fct] glyph atlas: texture creation/upload failed - using the game font");
        return false;
    }
    g_atlas.material = game::CloneMaterialWithTexture(gameMaterial, texture);
    if (!g_atlas.material) {
        Log("[fct] glyph atlas: material clone failed - using the game font");
        return false;
    }
    Log("[fct] glyph atlas: font '%ls' weight %d, cap %.0f px, cell %d px, outline %d, shadow %d (blur %d)",
        face, cfg.fontWeight, g_atlas.capH, kCellH, radius, shadowOff, blur);
    return true;
}

} // namespace

const Atlas* Get(void* gameMaterial)
{
    if (!g_tried) {
        g_tried = true;
        g_ok = gameMaterial && Build(gameMaterial);
    }
    return g_ok ? &g_atlas : nullptr;
}

} // namespace prk::fct::atlas
