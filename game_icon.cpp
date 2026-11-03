#include "game_icon.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

std::vector<uint8_t> FitLogoIcon(const art::Image& logo, uint32_t size)
{
    if (logo.width == 0 || logo.height == 0 || logo.rgba.size() != size_t(logo.width) * logo.height)
        return {};
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

namespace
{
    void Put(std::vector<uint8_t>& bytes, uint32_t value, unsigned count, bool big = false)
    {
        for (unsigned i = 0; i < count; ++i)
            bytes.push_back(uint8_t(value >> (8 * (big ? count - i - 1 : i))));
    }
}

std::vector<uint8_t> LogoIco(const art::Image& logo)
{
    constexpr unsigned sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
    std::vector<uint8_t> directory = { 0, 0, 1, 0, 7, 0 }, images;
    for (unsigned size : sizes)
    {
        auto png = FitLogoIcon(logo, size);
        if (png.empty())
            return {};
        directory.insert(directory.end(), { uint8_t(size), uint8_t(size), 0, 0 });
        Put(directory, 1, 2);
        Put(directory, 32, 2);
        Put(directory, uint32_t(png.size()), 4);
        Put(directory, uint32_t(6 + 16 * 7 + images.size()), 4);
        images.insert(images.end(), png.begin(), png.end());
    }
    directory.insert(directory.end(), images.begin(), images.end());
    return directory;
}

std::vector<uint8_t> LogoIcns(const art::Image& logo)
{
    std::vector<uint8_t> entries;
    const char* types[] = { "icp4", "icp5", "icp6", "ic07", "ic08", "ic09", "ic10", "ic11", "ic12", "ic13", "ic14" };
    const unsigned sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 32, 64, 256, 512 };
    for (unsigned i = 0; i < 11; ++i)
    {
        auto png = FitLogoIcon(logo, sizes[i]);
        if (png.empty())
            return {};
        entries.insert(entries.end(), types[i], types[i] + 4);
        Put(entries, uint32_t(8 + png.size()), 4, true);
        entries.insert(entries.end(), png.begin(), png.end());
    }
    std::vector<uint8_t> out = { 'i', 'c', 'n', 's' };
    Put(out, uint32_t(8 + entries.size()), 4, true);
    out.insert(out.end(), entries.begin(), entries.end());
    return out;
}
