// The game's icon from its disc logo: the logo cut to its visible pixels, fitted and centred in a transparent square.
// Shared by SkyBuild (game.txt "icon=logo", docs/tools/skybuild.md).
#pragma once

#include "host/art/xenos_texture.h"
#include <cstdint>
#include <vector>

// A PNG of `size` x `size` pixels: `logo` cut to the pixels whose alpha is visible, scaled to fit (a margin of size / 24 on
// each side, area-averaged on premultiplied colour) and centred on transparency. Empty when `logo` has no visible pixel.
std::vector<uint8_t> FitLogoIcon(const art::Image& logo, uint32_t size);
