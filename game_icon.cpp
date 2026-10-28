#include "game_icon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

std::vector<uint8_t> FitLogoIcon(const art::Image& logo, uint32_t size)
{
    constexpr uint32_t visible = 16; // alpha above this counts as part of the logo
    uint32_t left = logo.width, right = 0, top = logo.height, bottom = 0;
    for (uint32_t y = 0; y < logo.height; y++)
        for (uint32_t x = 0; x < logo.width; x++)
            if ((logo.rgba[size_t(y) * logo.width + x] >> 24) > visible)
            {
                left = std::min(left, x), right = std::max(right, x + 1);
                top = std::min(top, y), bottom = std::max(bottom, y + 1);
            }
    if (right <= left || bottom <= top || size < 8)
        return {};

    const double boxWidth = right - left, boxHeight = bottom - top;
    const double margin = size / 24.0;
    const double scale = (size - 2 * margin) / std::max(boxWidth, boxHeight); // output pixels per logo pixel
    const double drawWidth = boxWidth * scale, drawHeight = boxHeight * scale;
    const double originX = (size - drawWidth) / 2, originY = (size - drawHeight) / 2;

    std::vector<uint8_t> rgba(size_t(size) * size * 4, 0);
    for (uint32_t y = 0; y < size; y++)
    {
        for (uint32_t x = 0; x < size; x++)
        {
            // The logo pixels this output pixel covers.
            double x0 = (x - originX) / scale + left, x1 = (x + 1 - originX) / scale + left;
            double y0 = (y - originY) / scale + top, y1 = (y + 1 - originY) / scale + top;
            if (x1 <= left || x0 >= right || y1 <= top || y0 >= bottom)
                continue;
            x0 = std::max(x0, double(left)), x1 = std::min(x1, double(right));
            y0 = std::max(y0, double(top)), y1 = std::min(y1, double(bottom));
            double sum[4] = {}, weight = 0;
            for (uint32_t sy = uint32_t(y0); sy < std::ceil(y1) && sy < logo.height; sy++)
            {
                double wy = std::min(sy + 1.0, y1) - std::max(double(sy), y0);
                for (uint32_t sx = uint32_t(x0); sx < std::ceil(x1) && sx < logo.width; sx++)
                {
                    double w = wy * (std::min(sx + 1.0, x1) - std::max(double(sx), x0));
                    uint32_t p = logo.rgba[size_t(sy) * logo.width + sx];
                    double alpha = double(p >> 24);
                    sum[0] += w * alpha * double(p & 255), sum[1] += w * alpha * double((p >> 8) & 255);
                    sum[2] += w * alpha * double((p >> 16) & 255), sum[3] += w * alpha;
                    weight += w;
                }
            }
            if (weight <= 0 || sum[3] <= 0)
                continue;
            uint8_t* out = &rgba[(size_t(y) * size + x) * 4];
            for (int c = 0; c < 3; c++)
                out[c] = uint8_t(std::lround(std::min(255.0, sum[c] / sum[3])));
            out[3] = uint8_t(std::lround(std::min(255.0, sum[3] / weight)));
        }
    }

    std::vector<uint8_t> png;
    auto append = [](void* context, void* data, int count) {
        auto* bytes = static_cast<std::vector<uint8_t>*>(context);
        bytes->insert(bytes->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + count);
    };
    if (!stbi_write_png_to_func(append, &png, int(size), int(size), 4, rgba.data(), int(size) * 4))
        return {};
    return png;
}
