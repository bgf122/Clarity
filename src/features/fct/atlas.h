#pragma once

#include <cstdint>

// High-resolution glyph atlas for floating text, replacing the game's 128x64 12 px
// "spritefont.png". Each printable ASCII glyph has two cells: a black outline + soft shadow
// cell and a white fill cell. The fill is tinted by the sprite's vertex colour (the game's
// colour), so drawing all outline cells first and then all fill cells gives coloured text
// with a clean outline that never covers the neighbouring glyph.
namespace prk::fct::atlas {

struct Glyph {
    bool present = false;
    uint32_t outlineFrame = 0; // DiaBill frame code: (x << 24) | (y << 16) | (w << 8) | h
    uint32_t fillFrame = 0;
    float originX = 0.0f; // texels from the cell's left edge to the pen position
    float cellW = 0.0f;   // texels
    float advance = 0.0f; // texels
};

struct Atlas {
    void* material = nullptr; // clone of the game's floating-text material with our texture
    Glyph glyphs[128];
    float cellH = 0.0f;    // texels
    float baseline = 0.0f; // texels from the cell top
    float capH = 0.0f;     // height of digits/capitals in texels (what SizePx maps to)
    float spaceAdvance = 0.0f;
};

// Builds the atlas once (game thread, needs the renderer). gameMaterial is the material of a
// floating-text visual, used as the template. Returns null if building failed; it is not retried.
const Atlas* Get(void* gameMaterial);

} // namespace prk::fct::atlas
