#include "buff_paint.h"

#include "../../core/frame_paint.h"
#include "../../core/text_raster.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace prk::buffs::paint {

using gfx::Canvas;

namespace {

raster::Font g_font;
float g_fontScale = 0.0f;
bool g_fontOk = false;

struct TextMasks {
    raster::Mask fill, outline;
    float width = 0.0f;
};
std::unordered_map<std::wstring, TextMasks> g_text;
constexpr int kTextPad = 3;

const TextMasks& Text(const std::wstring& s)
{
    auto it = g_text.find(s);
    if (it != g_text.end())
        return it->second;
    if (g_text.size() > 512)
        g_text.clear();
    TextMasks t;
    t.width = g_font.TextWidth(s.c_str(), static_cast<int>(s.size()));
    const int w = static_cast<int>(std::ceil(t.width)) + 2 * kTextPad;
    const int h = static_cast<int>(std::ceil(g_font.Height())) + 2 * kTextPad;
    t.fill = raster::Rasterize(g_font, s.c_str(), static_cast<int>(s.size()), kTextPad, kTextPad, w, h);
    t.outline = raster::Dilate(t.fill, 1);
    return g_text.emplace(s, std::move(t)).first->second;
}

// Text centred on cx with its capitals' middle at cy.
void DrawCentred(Canvas& c, const std::wstring& s, float cx, float cy, uint32_t rgb)
{
    if (s.empty())
        return;
    const TextMasks& t = Text(s);
    const float top = cy - g_font.Ascent() * (1.0f - 0.36f);
    const int x0 = static_cast<int>(std::lround(cx - t.width * 0.5f)) - kTextPad, y0 = static_cast<int>(std::lround(top)) - kTextPad;
    c.Mask(x0, y0, t.outline, 0x000000, 0.95f);
    c.Mask(x0, y0, t.fill, rgb, 1.0f);
}

// The border round an icon at (x, y): a thin ring in the nano kind's colour, a dark edge outside it.
void Border(Canvas& c, const Metrics& m, int x, int y, uint32_t rgb)
{
    const float b = static_cast<float>(m.border), sw = static_cast<float>(m.iconW), sh = static_cast<float>(m.iconH);
    const float x0 = x - b, y0 = y - b, x1 = x + sw + b, y1 = y + sh + b;
    c.RoundRect(x0 - 1.0f, y0 - 1.0f, x1 + 1.0f, y1 + 1.0f, 0.0f, 0x050607, 0x050607, 1.0f);
    c.RoundRect(x0, y0, x1, y1, 0.0f, rgb, rgb, 1.0f);
    c.RoundRect(static_cast<float>(x), static_cast<float>(y), x + sw, y + sh, 0.0f, 0x020304, 0x020304, 1.0f);
}

} // namespace

Metrics MetricsFor(int iconW, int iconH)
{
    Metrics m;
    m.iconW = std::clamp(iconW, 8, 128);
    m.iconH = std::clamp(iconH, 8, 128);
    const float s = 1.0f; // the borders and text as at the game's own icon size
    m.border = std::max(1, frame::Px(1, s));
    m.gap = std::max(2, frame::Px(4, s));
    m.timeH = std::max(10, frame::Px(13, s));
    m.pad = std::max(4, frame::Px(6, s));
    m.rowGap = std::max(3, frame::Px(6, s));
    m.grip = std::max(10, frame::Px(12, s));
    return m;
}

void MinSize(const Metrics& m, int* w, int* h)
{
    *w = 2 * m.pad + m.CellW() - m.gap;
    *h = 2 * m.pad + m.CellH() - m.gap;
}

Layout Arrange(const Metrics& m, int w, int h, int buffs, int debuffs, bool rightToLeft)
{
    Layout l;
    int minW, minH;
    MinSize(m, &minW, &minH);
    l.w = std::max(w, minW);
    l.h = std::max(h, minH);
    const int cols = std::max(1, (l.w - 2 * m.pad + m.gap) / m.CellW());
    int y = m.pad;
    auto place = [&](int n, std::vector<POINT>& at) {
        for (int i = 0; i < n; ++i) {
            const int col = i % cols, row = i / cols;
            const int cy = y + row * m.CellH();
            if (cy + m.CellH() - m.gap > l.h - m.pad + 1)
                break;
            const int cx = rightToLeft ? l.w - m.pad - (col + 1) * m.CellW() + m.gap : m.pad + col * m.CellW();
            at.push_back({cx + m.border, cy + m.border});
        }
        if (n > 0)
            y += ((n + cols - 1) / cols) * m.CellH();
    };
    place(buffs, l.buffAt);
    if (buffs > 0 && debuffs > 0)
        y += m.rowGap;
    place(debuffs, l.debuffAt);
    return l;
}

bool EnsureFonts(float scale)
{
    if (scale == g_fontScale)
        return g_fontOk;
    g_font.Destroy();
    g_text.clear();
    g_fontScale = scale;
    g_fontOk = g_font.Create(L"Bahnschrift", std::max(9, frame::Px(12, scale)), 600);
    return g_fontOk;
}

void Paint(Canvas& c, const Metrics& m, const Layout& l, const std::vector<Buff>& buffs, const std::vector<Buff>& debuffs, bool unlocked)
{
    if (unlocked) {
        // The frame's area: a dim panel with a teal edge, and the grip in the bottom-right corner.
        const float W = static_cast<float>(l.w), H = static_cast<float>(l.h);
        c.RoundRect(0.0f, 0.0f, W, H, 4.0f, 0x0B0C0E, 0x0B0C0E, 0.55f);
        c.RoundRect(1.0f, 1.0f, W - 1.0f, H - 1.0f, 3.0f, 0x141619, 0x101214, 0.6f);
        for (int x = 2; x < l.w - 2; x += 6) {
            c.Line(static_cast<float>(x), 1.0f, std::min(W - 2.0f, x + 3.0f), 1.0f, 1.0f, frame::kTealRgb, 0.7f);
            c.Line(static_cast<float>(x), H - 1.5f, std::min(W - 2.0f, x + 3.0f), H - 1.5f, 1.0f, frame::kTealRgb, 0.7f);
        }
        for (int y = 2; y < l.h - 2; y += 6) {
            c.Line(1.0f, static_cast<float>(y), 1.0f, std::min(H - 2.0f, y + 3.0f), 1.0f, frame::kTealRgb, 0.7f);
            c.Line(W - 1.5f, static_cast<float>(y), W - 1.5f, std::min(H - 2.0f, y + 3.0f), 1.0f, frame::kTealRgb, 0.7f);
        }
        const float g = static_cast<float>(m.grip);
        for (int i = 0; i < 3; ++i) {
            const float d = 3.0f + i * g / 3.5f;
            c.Line(W - 2.0f - d, H - 2.0f, W - 2.0f, H - 2.0f - d, 1.5f, 0x9AA0A6, 0.9f);
        }
    }
    auto icons = [&](const std::vector<Buff>& list, const std::vector<POINT>& at) {
        for (size_t i = 0; i < at.size() && i < list.size(); ++i) {
            const Buff& b = list[i];
            Border(c, m, at[i].x, at[i].y, b.rgb);
            const uint32_t rgb = b.expiring ? 0xFFD24A : 0xF2F4F6;
            DrawCentred(c, b.time, at[i].x + m.iconW * 0.5f, at[i].y + m.iconH + m.border + m.timeH * 0.5f + 1.0f, rgb);
        }
    };
    icons(buffs, l.buffAt);
    icons(debuffs, l.debuffAt);
}

std::wstring TimeLeft(int cs)
{
    if (cs <= 0)
        return {};
    const int s = cs / 100;
    wchar_t t[16];
    if (s >= 3600)
        swprintf_s(t, L"%dh", (s + 1800) / 3600);
    else if (s >= 60)
        swprintf_s(t, L"%dm", (s + 30) / 60);
    else
        swprintf_s(t, L"%ds", s);
    return t;
}

} // namespace prk::buffs::paint
