// HD textures, the picture side (docs/tools/hd_textures.md): our own classical 2x pass for the game's textures (DXT
// deblocking, an edge-aware 2x, a light sharpening), the mip chain and the BC1/BC3 encoders the pack is written with.
// Plain C++ on art::Image (R in the low byte, straight alpha), no game or GPU code, so a test can use it.
#pragma once

#include "host/art/xenos_texture.h"
#include <cstdint>
#include <vector>

namespace hdtex
{
    // What a texture holds, as far as upscaling cares: only Color is upscaled. Tiny: a side under 32 texels or fewer
    // than 64x64. Flat: one colour. Normal: a normal map (unit vectors in RGB, or X and Y alone in two channels). Mask:
    // the colour carries no picture (grey only, or one colour under a varying alpha: fonts, glows, masks).
    enum class Kind
    {
        Color,
        Tiny,
        Flat,
        Normal,
        Mask,
    };
    const char* KindName(Kind kind);

    // Opaque: alpha 255 everywhere. Cutout: alpha 0 or 255 (nearly), drawn with an alpha test. Blend: graded alpha.
    enum class Alpha
    {
        Opaque,
        Cutout,
        Blend,
    };

    struct Analysis
    {
        Kind kind = Kind::Color;
        Alpha alpha = Alpha::Opaque;
    };
    Analysis Analyse(const art::Image& image);

    struct UpscaleOptions
    {
        bool deblock = true;  // the source was DXT: soften the 4x4 block seams first
        float warp = 2.0f;    // warp sharpening depth: how much a doubled edge's ramp is narrowed (0: off)
        float sharpen = 0.5f; // unsharp amount, clamped to the neighbourhood (no halos)
    };

    // Twice the size. Filtered in premultiplied alpha (no dark fringes); a cutout's alpha is thresholded again (crisp
    // edges, the same coverage). The output keeps the texel centres of the input (each texel becomes a 2x2 square).
    art::Image Upscale2x(const art::Image& image, Alpha alpha, const UpscaleOptions& options = {});

    // The levels below `top` down to 1x1, each half the one before (2x2 box in premultiplied alpha).
    std::vector<art::Image> MipChain(const art::Image& top);

    // BC1 and BC3 blocks, rows of 4x4 blocks, as a DDS file holds them (edges padded by repeating the last texel). With
    // `alpha`, BC1 makes a texel under 128 transparent (its 3-colour mode).
    std::vector<uint8_t> EncodeBc1(const art::Image& image, bool alpha);
    std::vector<uint8_t> EncodeBc3(const art::Image& image);
}
