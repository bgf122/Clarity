// Draws Clarity's action bars as the DLL paints them, without the game, to PNGs:
//
//     clarity_preview.exe <out dir> [icons dir]
//
// The game's own parts (the icons in the slots) are stand-ins: coloured squares, or the PNGs in [icons dir]
// (the game's own icons, taken from its rdb.db - kept out of the repository). Fonts are the system's
// Bahnschrift, as in game.
#include "../../src/core/canvas.h"
#include "../../src/features/actionbars/ab_paint.h"
#include "../../src/features/buffs/buff_paint.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using prk::gfx::Canvas;
namespace ab = prk::actionbars::paint;
namespace bp = prk::buffs::paint;

namespace {

bool SavePng(const Canvas& c, const std::wstring& path)
{
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(c.w, c.h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt));
    if (ok) {
        std::vector<uint8_t> px(static_cast<size_t>(c.w) * c.h * 4);
        for (size_t i = 0; i < static_cast<size_t>(c.w) * c.h; ++i) {
            const float* s = &c.px[i * 4];
            const float a = s[3];
            auto ch = [&](float v) { return static_cast<uint8_t>(std::clamp(a > 0 ? v / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f); };
            px[i * 4 + 0] = ch(s[2]);
            px[i * 4 + 1] = ch(s[1]);
            px[i * 4 + 2] = ch(s[0]);
            px[i * 4 + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        ok = SUCCEEDED(frame->WritePixels(c.h, c.w * 4, static_cast<UINT>(px.size()), px.data())) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(enc->Commit());
    }
    if (frame)
        frame->Release();
    if (enc)
        enc->Release();
    if (stream)
        stream->Release();
    f->Release();
    return ok;
}

// A backdrop like the game world behind the interface: a light sandy area and a dark green one.
void Backdrop(Canvas& c)
{
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            const bool light = x < c.w * 0.35f + (c.h - y) * 0.35f;
            const uint32_t rgb = light ? Canvas::Mix(0xE6E0D2, 0xD4CCBA, static_cast<float>(y) / c.h)
                                       : Canvas::Mix(0x2C3A33, 0x1A221E, static_cast<float>(y) / c.h);
            c.Blend(x, y, rgb, 1.0f);
        }
}

// A stand-in for the game's 32x32 icon: a coloured square with a lighter mark.
void FakeIcon(Canvas& c, int x, int y, int size, uint32_t rgb, int height = 0, bool greyFrame = true)
{
    const float s = static_cast<float>(size), t = static_cast<float>(height > 0 ? height : size);
    // The game's item icons have a frame of their own: a grey border (nano icons don't).
    const float in = greyFrame ? 2.0f : 0.0f;
    if (greyFrame)
        c.RoundRect(static_cast<float>(x), static_cast<float>(y), x + s, y + t, 1.0f, 0x8C9196, 0x5A5F64, 1.0f);
    c.RoundRect(x + in, y + in, x + s - in, y + t - in, greyFrame ? 1.0f : 0.0f, Canvas::Mix(rgb, 0xFFFFFF, 0.25f), prk::gfx::Darker(rgb, 0.55f), 1.0f);
    const float m = std::min(s, t);
    const float cx = x + s * 0.5f, cy = y + t * 0.5f;
    c.Line(cx - m * 0.25f, cy + m * 0.2f, cx, cy - m * 0.2f, m * 0.12f, Canvas::Mix(rgb, 0xFFFFFF, 0.7f), 0.9f);
    c.Line(cx, cy - m * 0.2f, cx + m * 0.25f, cy + m * 0.2f, m * 0.12f, Canvas::Mix(rgb, 0xFFFFFF, 0.7f), 0.9f);
}

// The game's icons, when a folder of them is given: straight ARGB, the colour key (pure green) clear.
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> argb;
};
std::vector<Image> g_icons;

bool LoadPng(const std::wstring& path, Image* out)
{
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, h = 0;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&w, &h));
    if (ok) {
        out->w = static_cast<int>(w), out->h = static_cast<int>(h);
        out->argb.resize(static_cast<size_t>(w) * h);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out->argb.size() * 4), reinterpret_cast<BYTE*>(out->argb.data())));
        for (uint32_t& v : out->argb)
            if ((v & 0xFFFFFF) == 0x00FF00)
                v = 0;
    }
    if (conv)
        conv->Release();
    if (frame)
        frame->Release();
    if (dec)
        dec->Release();
    f->Release();
    return ok;
}

void LoadIcons(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        Image im;
        if (LoadPng(dir + L"\\" + fd.cFileName, &im) && im.w == im.h)
            g_icons.push_back(std::move(im));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// An icon drawn size x size (each pixel the average of the source pixels it covers, as a downscale).
void DrawIcon(Canvas& c, const Image& im, int x, int y, int size)
{
    const float k = static_cast<float>(im.w) / size;
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            float r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = static_cast<int>(j * k); sy < static_cast<int>(std::ceil((j + 1) * k)) && sy < im.h; ++sy)
                for (int sx = static_cast<int>(i * k); sx < static_cast<int>(std::ceil((i + 1) * k)) && sx < im.w; ++sx) {
                    const uint32_t v = im.argb[static_cast<size_t>(sy) * im.w + sx];
                    const float al = (v >> 24) / 255.0f;
                    r += ((v >> 16) & 0xFF) * al, g += ((v >> 8) & 0xFF) * al, b += (v & 0xFF) * al, a += al, n += 1;
                }
            if (a <= 0.0f)
                continue;
            const uint32_t rgb = (static_cast<uint32_t>(r / a) << 16) | (static_cast<uint32_t>(g / a) << 8) | static_cast<uint32_t>(b / a);
            c.Blend(x + i, y + j, rgb, a / n);
        }
}

const uint32_t kIconRgb[] = {0xB8432F, 0x3C7FC4, 0x5FA840, 0xC79A2A, 0x8A4FC0, 0x2FA7A0, 0xC45E8A, 0x7A7F3A, 0x4B6CD8, 0xD27A2E};

// The icons the game would draw in a row's filled slots.
void Icons(Canvas& c, const ab::Metrics& m, int x, int y, const ab::Row& r, int seed)
{
    for (int i = 0; i < ab::kSlots; ++i) {
        if (!r.slot[i].filled)
            continue;
        int cx, cy;
        ab::CellAt(m, r.vertical, x, y, i, &cx, &cy);
        if (!g_icons.empty())
            DrawIcon(c, g_icons[(i * 7 + seed * 3) % g_icons.size()], cx + m.margin, cy + m.margin, m.icon);
        else
            FakeIcon(c, cx + m.margin, cy + m.margin, m.icon, kIconRgb[(i + seed) % 10]);
    }
}

ab::Dock SampleDock(bool hideEmpty)
{
    ab::Dock d;
    d.rows = 2;
    d.hideEmpty = hideEmpty;
    // The first bar: the active one, keys 1-0, a few empty slots.
    const bool filled0[ab::kSlots] = {true, true, true, true, true, true, false, true, false, true};
    for (int i = 0; i < ab::kSlots; ++i) {
        d.row[0].slot[i].filled = filled0[i];
        d.row[0].slot[i].key = ab::PageLabel(i);
    }
    d.row[0].active = true;
    d.row[0].page = 0;
    // The second bar: keys bound by hand to some slots.
    const bool filled1[ab::kSlots] = {true, true, true, true, false, false, false, false, true, true};
    const wchar_t* keys1[ab::kSlots] = {L"s1", L"s2", L"s3", L"s4", L"", L"", L"", L"", L"F1", L"F2"};
    for (int i = 0; i < ab::kSlots; ++i) {
        d.row[1].slot[i].filled = filled1[i];
        d.row[1].slot[i].key = keys1[i];
    }
    d.row[1].page = 2;
    d.xp = {true, 0.5f, L"1,000,500 / 2,001,000", ab::kGameXpRgb, ab::kGameXpShading};
    d.alien = {true, 0.47f, L"118,910 / 253,000", ab::kGameAlienRgb, ab::kGameAlienShading};
    return d;
}

void Scene(const std::wstring& path, float cellScale, bool hideEmpty)
{
    const ab::Metrics m = ab::MetricsFor(36.0f * cellScale, 32.0f * cellScale);
    ab::EnsureFonts(m.scale);
    const ab::Dock d = SampleDock(hideEmpty);
    const ab::DockLayout l = ab::LayoutDock(m, d.rows, d.xp.shown, d.alien.shown);

    ab::Row side;
    side.vertical = true;
    const bool filledSide[ab::kSlots] = {true, true, true, false, true, false, false, false, false, false};
    const wchar_t* keysSide[ab::kSlots] = {L"c1", L"c2", L"", L"", L"a5", L"", L"", L"", L"", L""};
    for (int i = 0; i < ab::kSlots; ++i) {
        side.slot[i].filled = filledSide[i];
        side.slot[i].key = keysSide[i];
    }
    int sw, sh;
    ab::RowSize(m, side, &sw, &sh);

    Canvas c;
    const int pad = static_cast<int>(40 * cellScale);
    c.Reset(l.w + sw + 3 * pad, std::max(l.h, sh) + 2 * pad);
    Backdrop(c);

    // The dock: its frame and slots, the game's icons over them, then the key labels over those.
    Canvas dock;
    dock.Reset(l.w, l.h);
    ab::PaintDock(dock, m, l, d);
    const int dx = pad, dy = c.h - pad - l.h;
    c.Paste(dock, dx, dy);
    for (int i = 0; i < d.rows; ++i) {
        Icons(c, m, dx + l.cellX[i], dy + l.rowY[i], d.row[i], i * 3);
        Canvas keys;
        int rw, rh;
        ab::RowSize(m, d.row[i], &rw, &rh);
        keys.Reset(rw, rh);
        ab::PaintKeys(keys, m, 0, 0, d.row[i], d.hideEmpty);
        c.Paste(keys, dx + l.cellX[i], dy + l.rowY[i]);
    }

    // A floating bar, standing up: its slots, icons and keys.
    const int sx = c.w - pad - sw, sy = c.h - pad - sh;
    Canvas back;
    back.Reset(sw, sh);
    ab::PaintSlots(back, m, 0, 0, side, hideEmpty);
    c.Paste(back, sx, sy);
    Icons(c, m, sx, sy, side, 5);
    Canvas keys;
    keys.Reset(sw, sh);
    ab::PaintKeys(keys, m, 0, 0, side, hideEmpty);
    c.Paste(keys, sx, sy);

    if (!SavePng(c, path))
        fwprintf(stderr, L"couldn't write %ls\n", path.c_str());
    else
        wprintf(L"%ls\n", path.c_str());
}

// The buff frame: 9 buffs and 3 debuffs over the game world, its area 8 icons wide and 3 rows high
// (unlocked: its panel and grip show), each sorted as in game: no end first, then the longest time left,
// the next to end last. blink: the expiring ones (under 10 s) at that point of the blink's fade (1 = full).
// rows: the frame's set height in rows (fewer than the nanos need: it grows to fit them).
void BuffScene(const std::wstring& path, bool unlocked, bool rightToLeft = false, float blink = 1.0f, int rows = 3)
{
    const bp::Metrics m = bp::MetricsFor(40, 24); // the game's nano icons
    bp::EnsureFonts(1.0f);
    // The border colours: the fallbacks for the game's kinds (in game, read from its NCU overlays).
    constexpr uint32_t kSelf = 0xE0B83A, kTeam = 0x58C85A, kFriendly = 0xC8CDD2, kHostile = 0xD8402F;
    std::vector<bp::Buff> buffs = {{false, kSelf, L""},     {false, kSelf, L"2h"},       {false, kTeam, L"58m"},
                                   {false, kSelf, L"45m"},  {false, kFriendly, L"12m"},  {false, kSelf, L"9m"},
                                   {false, kTeam, L"3m"},   {false, kFriendly, L"58s"},  {false, kSelf, L"7s", true}};
    std::vector<bp::Buff> debuffs = {{true, kHostile, L"25s"}, {true, kHostile, L"12s"}, {true, kHostile, L"4s", true}};
    for (auto* list : {&buffs, &debuffs})
        for (bp::Buff& b : *list)
            b.alpha = b.expiring ? blink : 1.0f;
    const int w = 2 * m.pad + 8 * m.CellW() - m.gap, h = 2 * m.pad + rows * m.CellH() - m.gap + m.rowGap;
    const bp::Layout l = bp::Arrange(m, w, h, static_cast<int>(buffs.size()), static_cast<int>(debuffs.size()), rightToLeft);
    Canvas c;
    const int pad = 40;
    c.Reset(l.w + 2 * pad, l.h + 2 * pad);
    Backdrop(c);
    Canvas f;
    f.Reset(l.w, l.h);
    bp::Paint(f, m, l, buffs, debuffs, unlocked);
    c.Paste(f, pad, pad);
    // An icon, faded as the game's SetAlpha fades it in the blink.
    auto icon = [&](const bp::Buff& b, POINT at, uint32_t rgb) {
        Canvas ic;
        ic.Reset(m.iconW, m.iconH);
        FakeIcon(ic, 0, 0, m.iconW, rgb, m.iconH, false);
        for (float& v : ic.px)
            v *= b.alpha;
        c.Paste(ic, pad + at.x, pad + at.y);
    };
    for (size_t i = 0; i < l.buffAt.size(); ++i)
        icon(buffs[i], l.buffAt[i], kIconRgb[(i * 3) % 10]);
    for (size_t i = 0; i < l.debuffAt.size(); ++i)
        icon(debuffs[i], l.debuffAt[i], kIconRgb[(i * 7 + 1) % 10]);
    if (!SavePng(c, path))
        fwprintf(stderr, L"couldn't write %ls\n", path.c_str());
    else
        wprintf(L"%ls\n", path.c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        fwprintf(stderr, L"usage: clarity_preview <out dir> [icons dir]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring dir = argv[1];
    CreateDirectoryW(dir.c_str(), nullptr);
    if (argc >= 3)
        LoadIcons(argv[2]);
    Scene(dir + L"\\actionbars.png", 1.0f, false);
    Scene(dir + L"\\actionbars_hide_empty.png", 1.0f, true);
    Scene(dir + L"\\actionbars_large.png", 1.5f, false);
    BuffScene(dir + L"\\buffs.png", false);
    BuffScene(dir + L"\\buffs_unlocked.png", true);
    BuffScene(dir + L"\\buffs_right_to_left.png", false, true);
    BuffScene(dir + L"\\buffs_blink_half.png", false, false, 0.65f);
    BuffScene(dir + L"\\buffs_blink_low.png", false, false, bp::kBlinkAlpha);
    BuffScene(dir + L"\\buffs_grown.png", true, false, 1.0f, 1);
    CoUninitialize();
    return 0;
}
