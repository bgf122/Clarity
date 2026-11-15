#pragma once

// Clarity's frame look, painted in software at any scale: the frames round your bars and targets, and
// the action bars' frame and slots. Every size is in pixels at Scale 1 and multiplied by `scale`.
#include "canvas.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace prk::frame {

constexpr uint32_t kTealRgb = 0x2FD3B9;
constexpr uint32_t kLabelRgb = 0xD2D6DA;

int Px(float v, float scale); // v * scale, rounded

int Outline(float scale);  // the dark outline round the frame
int Ring(float scale);     // the gunmetal ring inside it
int Inner(float scale);    // the frame's edge -> the inner panel
float Cut(float scale);    // the corners' cut
float InnerCut(float cut, float d); // the cut of the same corner d pixels further in (its edges parallel)
int Bezel(float scale);    // the silver bezel round a bar

// A teal line with a soft glow along the points.
void TealPath(gfx::Canvas& c, const std::vector<std::pair<float, float>>& pts, float alpha, float scale);

// A frame filling the canvas: a dark outline with cut corners, a gunmetal ring with steps along its top
// and bottom, a dark inner panel, teal lines along the lower inner corners and a teal light on the top
// right.
void PaintFrame(gfx::Canvas& c, float scale);

// The inner panel's shape in a W x H frame: the signed distance (pixels, < 0 inside) from a point to it.
float InnerDistance(float W, float H, float scale, float px, float py);
// The step along the middle of the top of the ring, reaching down into the inner panel by TopStepDepth:
// painted again over anything set flush against the panel's top, so the frame overlaps it.
void PaintTopStep(gfx::Canvas& c, float scale);
float TopStepDepth(float scale);

// The fill's shading from top to bottom: a bright edge, a light band fading down, the colour, and deeper
// at the bottom with a bright edge again.
std::vector<uint32_t> Gloss(uint32_t rgb);

// A bar set into a frame, its inside at (x, y), w x h: BarBezel paints the silver bezel round it on the
// frame; BarBody is the inside (the track, the filled part - fraction 0..1, < 0 = unknown - shaded top to
// bottom by `shading`, else Gloss(rgb), with a bright end, and section lines every `section` pixels, 0 =
// none), for the caller to write on and paste at (x, y).
void BarBezel(gfx::Canvas& c, int x, int y, int w, int h, float scale);
gfx::Canvas BarBody(int w, int h, float fraction, uint32_t rgb, float section = 0.0f, const std::vector<uint32_t>* shading = nullptr);

} // namespace prk::frame
