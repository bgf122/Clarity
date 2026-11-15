#include "ab_paint.h"

#include "../../core/frame_paint.h"
#include "../../core/text_raster.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace prk::actionbars::paint {

using gfx::Canvas;

namespace {

int Px(float v, float scale)
{
    return frame::Px(v, scale);
}

// --- text -----------------------------------------------------------------------------------

enum FontId { kFontKey, kFontPage, kFontXp, kFontCount };
raster::Font g_fonts[kFontCount];
float g_fontScale = 0.0f;
bool g_fontsOk = false;

struct TextMasks {
    raster::Mask fill, outline, outline2; // outline2: two pixels wide
    float width = 0.0f;
};
std::unordered_map<std::wstring, TextMasks> g_text;
constexpr int kTextPad = 3;

const TextMasks& Text(FontId font, const std::wstring& s)
{
    std::wstring key(1, static_cast<wchar_t>(L'0' + font));
    key += s;
    auto it = g_text.find(key);
    if (it != g_text.end())
        return it->second;
    if (g_text.size() > 512)
        g_text.clear();
    const raster::Font& f = g_fonts[font];
    TextMasks t;
    t.width = f.TextWidth(s.c_str(), static_cast<int>(s.size()));
    const int w = static_cast<int>(std::ceil(t.width)) + 2 * kTextPad;
    const int h = static_cast<int>(std::ceil(f.Height())) + 2 * kTextPad;
    t.fill = raster::Rasterize(f, s.c_str(), static_cast<int>(s.size()), kTextPad, kTextPad, w, h);
    t.outline = raster::Dilate(t.fill, 1);
    t.outline2 = raster::Dilate(t.fill, 2);
    return g_text.emplace(std::move(key), std::move(t)).first->second;
}

float TextWidth(FontId font, const std::wstring& s)
{
    return s.empty() ? 0.0f : Text(font, s).width;
}

// Top of a line whose capitals and digits are centred on y.
float CentredTop(FontId font, float y)
{
    return y - g_fonts[font].Ascent() * (1.0f - 0.36f);
}

// The top of the capitals from the text's top.
float CapTop(FontId font)
{
    return g_fonts[font].Ascent() * 0.28f;
}

void DrawText(Canvas& c, FontId font, const std::wstring& s, float x, float y, uint32_t rgb, float outline = 0.9f)
{
    if (s.empty())
        return;
    const TextMasks& t = Text(font, s);
    const int x0 = static_cast<int>(std::lround(x)) - kTextPad, y0 = static_cast<int>(std::lround(y)) - kTextPad;
    c.Mask(x0, y0, t.outline, 0x000000, outline);
    c.Mask(x0, y0, t.fill, rgb, 1.0f);
}

// Text over a bar filled up to fillX: dark letters over the fill (a bright colour), light letters with a
// dark outline over the empty track - each part only on its side.
void DrawBarText(Canvas& c, FontId font, const std::wstring& s, float x, float y, float fillX)
{
    if (s.empty())
        return;
    const TextMasks& t = Text(font, s);
    const int x0 = static_cast<int>(std::lround(x)) - kTextPad, y0 = static_cast<int>(std::lround(y)) - kTextPad;
    auto layer = [&](const raster::Mask& m, uint32_t rgb, float alpha, bool onFill) {
        for (int my = 0; my < m.h; ++my)
            for (int mx = 0; mx < m.w; ++mx) {
                // How much of the pixel is on the wanted side of the fill's end.
                const float filled = std::clamp(fillX - (x0 + mx), 0.0f, 1.0f);
                const float side = onFill ? filled : 1.0f - filled;
                if (side > 0.0f)
                    c.Blend(x0 + mx, y0 + my, rgb, m.a[static_cast<size_t>(my) * m.w + mx] * alpha * side);
            }
    };
    layer(t.outline, 0xFFF4C8, 0.35f, true); // a faint light edge keeps the dark letters crisp on the fill
    layer(t.fill, 0x15120A, 1.0f, true);
    layer(t.outline, 0x000000, 0.95f, false);
    layer(t.fill, 0xF2F4F6, 1.0f, false);
}

// --- slots ----------------------------------------------------------------------------------

// A slot's bezel round its icon (ix, iy): a dark edge, a thin silver ring lit from above, and the well
// the icon sits in (an empty slot shows it: dark, a faint light along its lower edge).
void PaintSlot(Canvas& c, const Metrics& m, float ix, float iy, bool filled)
{
    const float s = static_cast<float>(m.icon);
    const float g = static_cast<float>(m.margin);
    c.RoundRect(ix - g, iy - g, ix + s + g, iy + s + g, g + 1.5f, 0x08090B, 0x08090B, 1.0f);
    c.RoundRect(ix - g + 1.0f, iy - g + 1.0f, ix + s + g - 1.0f, iy + s + g - 1.0f, g + 0.5f, 0x9AA0A6, 0x3F4347, 1.0f);
    c.RoundRect(ix, iy, ix + s, iy + s, 1.0f, 0x020304, 0x020304, 1.0f);
    if (filled)
        return;
    c.RoundRect(ix + 1.0f, iy + 1.0f, ix + s - 1.0f, iy + s - 1.0f, 0.5f, 0x16191C, 0x0B0C0E, 1.0f);
    for (int x = static_cast<int>(ix) + 2; x < static_cast<int>(ix + s) - 2; ++x)
        c.Blend(x, static_cast<int>(iy + s) - 2, 0x5A6066, 0.35f);
    // A faint teal glint in the top-left corner, as the frames' lights.
    c.Line(ix + 3.0f, iy + 3.0f, ix + 3.0f + Px(6, m.scale), iy + 3.0f, 1.0f, frame::kTealRgb, 0.25f);
}

// --- the page control -----------------------------------------------------------------------

// A chevron pointing up or down, centred on (x, y).
void Chevron(Canvas& c, float x, float y, float size, bool up, uint32_t rgb, float alpha)
{
    const float h = size * 0.5f, d = up ? -1.0f : 1.0f;
    const float w = std::max(1.2f, size * 0.22f);
    c.Line(x - size * 0.6f, y - d * h * 0.5f, x, y + d * h * 0.5f, w, rgb, alpha);
    c.Line(x, y + d * h * 0.5f, x + size * 0.6f, y - d * h * 0.5f, w, rgb, alpha);
}

void PaintPage(Canvas& c, const Metrics& m, const RECT& r, const Row& row)
{
    const float x0 = static_cast<float>(r.left), y0 = static_cast<float>(r.top);
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    // A recess like the slots', narrower.
    c.RoundRect(x0, y0, x0 + w, y0 + h, 3.0f, 0x08090B, 0x08090B, 1.0f);
    c.RoundRect(x0 + 1.0f, y0 + 1.0f, x0 + w - 1.0f, y0 + h - 1.0f, 2.0f, 0x15171A, 0x0D0E10, 1.0f);
    const float cx = x0 + w * 0.5f, chev = static_cast<float>(std::max(5, Px(7, m.scale)));
    Chevron(c, cx, y0 + h * 0.17f, chev, true, 0x9AA0A6, 0.95f);
    Chevron(c, cx, y0 + h * 0.83f, chev, false, 0x9AA0A6, 0.95f);
    const std::wstring label = PageLabel(row.page);
    const uint32_t rgb = row.active ? frame::kTealRgb : frame::kLabelRgb;
    DrawText(c, kFontPage, label, std::round(cx - TextWidth(kFontPage, label) * 0.5f), CentredTop(kFontPage, y0 + h * 0.5f), rgb);
}

// --- the XP bars ----------------------------------------------------------------------------

// An XP bar across the frame's inner panel, cut to the panel's shape (its corners), with a dark line under
// it; `covered` pixels of its top are under the frame's top step, so its text is centred on the rest.
void PaintXp(Canvas& c, const Metrics& m, const RECT& r, const Xp& xp, float covered)
{
    if (!xp.shown || r.right <= r.left)
        return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    Canvas bar = frame::BarBody(w, h, xp.fraction, xp.rgb, static_cast<float>(w) / 20.0f, xp.shading.empty() ? nullptr : &xp.shading);
    for (int x = 0; x < w; ++x) // a dark line along its bottom, between it and what's under it
        bar.Blend(x, h - 1, 0x050607, 0.85f);
    if (!xp.text.empty())
        DrawBarText(bar, kFontXp, xp.text, std::round((w - TextWidth(kFontXp, xp.text)) * 0.5f),
                    CentredTop(kFontXp, (covered + h - 1) * 0.5f) + 0.5f, xp.fraction > 0.0f ? w * std::min(xp.fraction, 1.0f) : 0.0f);
    // Only inside the inner panel.
    const float W = static_cast<float>(c.w), H = static_cast<float>(c.h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float d = frame::InnerDistance(W, H, m.scale, r.left + x + 0.5f, r.top + y + 0.5f);
            const float keep = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (keep < 1.0f) {
                float* p = &bar.px[(static_cast<size_t>(y) * w + x) * 4];
                for (int i = 0; i < 4; ++i)
                    p[i] *= keep;
            }
        }
    c.Paste(bar, r.left, r.top);
}

} // namespace

const std::vector<uint32_t> kGameXpShading = {0xA28303, 0xC29C03, 0xD0A703, 0xE4B803, 0xF6D966, 0xE4B803, 0xD0A703, 0xC29C03, 0xA28303};
const std::vector<uint32_t> kGameAlienShading = {0x1F9703, 0x24B503, 0x26C103, 0x2AD503, 0x3AF510, 0x2AD503, 0x26C103, 0x24B503, 0x1F9703};

Metrics MetricsFor(float cell, float icon, float margin)
{
    Metrics m;
    m.cellF = std::max(8.0f, cell);
    m.cell = static_cast<int>(std::lround(m.cellF));
    m.icon = std::clamp(static_cast<int>(std::lround(icon)), 4, m.cell);
    m.margin = margin >= 0.0f ? std::clamp(static_cast<int>(std::lround(margin)), 0, m.cell - m.icon) : (m.cell - m.icon) / 2;
    m.scale = m.cellF / 36.0f;
    const float s = m.scale;
    m.inset = frame::Inner(s) + Px(8, s);
    m.rowGap = Px(6, s);
    m.pageGap = Px(6, s);
    m.pageW = std::max(14, Px(20, s));
    // Flush against the frame's top: the XP bar a little taller, as the frame's top step covers some of it.
    m.xpTopH = std::max(10, Px(16, s));
    m.xpH = std::max(8, Px(13, s));
    m.xpBetween = Px(2, s);
    m.xpGap = Px(8, s);
    return m;
}

// From the first cell's start to the last cell's end.
int RowLength(const Metrics& m)
{
    return static_cast<int>(std::lround((kSlots - 1) * m.cellF)) + m.cell;
}

void CellAt(const Metrics& m, bool vertical, int x, int y, int i, int* cx, int* cy)
{
    const int d = static_cast<int>(std::lround(i * m.cellF));
    *cx = vertical ? x : x + d;
    *cy = vertical ? y + d : y;
}

std::wstring PageLabel(int page)
{
    return std::to_wstring((page + 1) % 10);
}

void RowSize(const Metrics& m, const Row& r, int* w, int* h)
{
    const int len = RowLength(m);
    *w = r.vertical ? m.cell : len;
    *h = r.vertical ? len : m.cell;
}

DockLayout LayoutDock(const Metrics& m, int rows, bool xpShown, bool alienShown)
{
    rows = std::clamp(rows, 1, 4);
    DockLayout l;
    // The frame's outline and ring take the corners' cut into account: keep the cells clear of it.
    const int left = m.inset + static_cast<int>(frame::Cut(m.scale) * 0.3f);
    const int len = RowLength(m);
    const int midGap = Px(14, m.scale);
    const bool twoColumns = rows >= 2;
    // Two bars side by side, each with its page control on the outer end: [page][bar 1]  [bar 2][page].
    const int cells0 = left + m.pageW + m.pageGap;
    const int cells1 = cells0 + len + midGap;
    const int right = twoColumns ? cells1 + len + m.pageGap + m.pageW : cells0 + len;

    l.w = right + left;

    // The XP bars on top: the XP bar flush against the inner panel's top, across all of it (the frame's
    // corners and top step cut into it), the alien XP bar under it.
    const int inner = frame::Inner(m.scale);
    int y = m.inset + Px(2, m.scale);
    if (xpShown) {
        y = inner;
        l.xp = {inner, y, l.w - inner, y + m.xpTopH};
        y += m.xpTopH;
        if (alienShown) {
            y += m.xpBetween;
            l.alien = {inner, y, l.w - inner, y + m.xpH};
            y += m.xpH;
        }
        y += m.xpGap;
    }

    // Then the bars, two to a line, the first two on the bottom line.
    const int lines = (rows + 1) / 2;
    for (int i = 0; i < rows; ++i) {
        const int line = lines - 1 - i / 2;
        l.rowY[i] = y + line * (m.cell + m.rowGap);
        const bool second = (i % 2) == 1;
        l.cellX[i] = second ? cells1 : cells0;
        const int px = second ? cells1 + len + m.pageGap : left;
        l.page[i] = {px, l.rowY[i] + m.margin, px + m.pageW, l.rowY[i] + m.cell - m.margin};
    }
    l.h = y + lines * m.cell + (lines - 1) * m.rowGap + m.inset + Px(4, m.scale);
    return l;
}

bool EnsureFonts(float scale)
{
    if (scale == g_fontScale)
        return g_fontsOk;
    for (raster::Font& f : g_fonts)
        f.Destroy();
    g_text.clear();
    g_fontScale = scale;
    g_fontsOk = g_fonts[kFontKey].Create(L"Bahnschrift", std::max(9, Px(12, scale)), 600) &&
                g_fonts[kFontPage].Create(L"Bahnschrift", std::max(10, Px(14, scale)), 700) &&
                g_fonts[kFontXp].Create(L"Bahnschrift", std::max(8, Px(11, scale)), 600);
    return g_fontsOk;
}

void PaintSlots(Canvas& c, const Metrics& m, int x, int y, const Row& r, bool hideEmpty)
{
    for (int i = 0; i < kSlots; ++i) {
        if (hideEmpty && !r.slot[i].filled)
            continue;
        int cx, cy;
        CellAt(m, r.vertical, x, y, i, &cx, &cy);
        PaintSlot(c, m, static_cast<float>(cx + m.margin), static_cast<float>(cy + m.margin), r.slot[i].filled);
    }
}

void PaintKeys(Canvas& c, const Metrics& m, int x, int y, const Row& r, bool hideEmpty)
{
    for (int i = 0; i < kSlots; ++i) {
        const Slot& slot = r.slot[i];
        if (slot.key.empty() || (hideEmpty && !slot.filled))
            continue;
        int cx, cy;
        CellAt(m, r.vertical, x, y, i, &cx, &cy);
        const float right = static_cast<float>(cx + m.margin + m.icon) - Px(2, m.scale);
        const float top = static_cast<float>(cy + m.margin) + Px(2, m.scale) - CapTop(kFontKey);
        const float tw = TextWidth(kFontKey, slot.key);
        const float tx = std::round(right - tw), ty = std::round(top);
        // Soft white on a heavy (two-pixel) black outline: readable over any icon without glaring.
        const TextMasks& t = Text(kFontKey, slot.key);
        const int x0 = static_cast<int>(tx) - kTextPad, y0 = static_cast<int>(ty) - kTextPad;
        c.Mask(x0, y0, t.outline2, 0x000000, 1.0f);
        c.Mask(x0, y0, t.fill, 0xDDE1E5, 1.0f);
    }
    // Every pixel fully there or not at all, as the game's own text (its GUI format has one bit of alpha):
    // the GUI draws its windows in texture order, resolving what covers what with its depth buffer and
    // an alpha test, so a half-clear pixel blends with whatever happens to be drawn already - the world,
    // if the frame under it comes later - and then keeps the frame from being drawn there. The letters stay
    // smooth against their outline (both opaque); only the outline's outer edge is hard.
    for (size_t i = 0; i + 3 < c.px.size(); i += 4) {
        float* p = &c.px[i];
        const float a = p[3];
        if (a <= 0.0f)
            continue;
        const float k = a >= 0.45f ? 1.0f / a : 0.0f; // premultiplied: straight colour at full alpha, or nothing
        for (int ch = 0; ch < 4; ++ch)
            p[ch] *= k;
    }
}

RECT XpArea(const Metrics& m, const DockLayout& l)
{
    (void)m;
    if (l.xp.right <= l.xp.left)
        return {0, 0, 0, 0};
    const RECT& last = l.alien.right > l.alien.left ? l.alien : l.xp;
    return {0, 0, l.w, last.bottom + 3}; // the frame's top too: it's painted over the XP bar
}

void PaintDock(Canvas& c, const Metrics& m, const DockLayout& l, const Dock& d)
{
    frame::PaintFrame(c, m.scale);
    for (int i = 0; i < std::clamp(d.rows, 1, 4); ++i) {
        PaintSlots(c, m, l.cellX[i], l.rowY[i], d.row[i], d.hideEmpty);
        PaintPage(c, m, l.page[i], d.row[i]);
    }
    PaintXp(c, m, l.xp, d.xp, frame::TopStepDepth(m.scale));
    PaintXp(c, m, l.alien, d.alien, 0.0f);
    frame::PaintTopStep(c, m.scale); // the frame over the XP bar
}

} // namespace prk::actionbars::paint
