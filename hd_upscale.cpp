// See hd_upscale.h. Our own code: the BC1/BC3 layouts follow the public block format description; the filters are
// textbook ones (Catmull-Rom, a structure tensor, an unsharp mask), written here.
#include "hd_upscale.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace hdtex
{
    namespace
    {
        // RGBA in 0..1, premultiplied, interleaved.
        struct Plane
        {
            uint32_t width = 0, height = 0;
            std::vector<float> texels;
            Plane() = default;
            Plane(uint32_t w, uint32_t h) : width(w), height(h), texels(size_t(w) * h * 4, 0.0f) {}
            float* At(uint32_t x, uint32_t y) { return &texels[(size_t(y) * width + x) * 4]; }
            const float* At(uint32_t x, uint32_t y) const { return &texels[(size_t(y) * width + x) * 4]; }
        };

        float Channel(uint32_t texel, int c) { return float((texel >> (8 * c)) & 0xFF) * (1.0f / 255.0f); }

        uint32_t Byte(float value) { return uint32_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); }

        Plane Premultiplied(const art::Image& image, bool opaque)
        {
            Plane plane(image.width, image.height);
            for (size_t i = 0; i < image.rgba.size(); i++)
            {
                float a = opaque ? 1.0f : Channel(image.rgba[i], 3);
                float* t = &plane.texels[i * 4];
                for (int c = 0; c < 3; c++)
                    t[c] = Channel(image.rgba[i], c) * a;
                t[3] = a;
            }
            return plane;
        }

        // A coordinate inside [0, n): wrapped for a picture that tiles, else clamped to the edge.
        int Address(int x, int n, bool wrap)
        {
            if (wrap)
                return ((x % n) + n) % n;
            return std::clamp(x, 0, n - 1);
        }

        float Difference(const float* a, const float* b)
        {
            return std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]) + std::fabs(a[3] - b[3]);
        }

        // Whether the picture tiles along x (columns) or y (rows): its last column (row) is as close to its first as
        // neighbouring ones are to each other. A tiling world texture is then filtered across its edge, so its copies
        // still join; a sprite or a UI picture is clamped (nothing from the far side bleeds in).
        bool Tiles(const Plane& p, bool alongX)
        {
            uint32_t n = alongX ? p.height : p.width, last = (alongX ? p.width : p.height) - 1;
            if (last < 2)
                return false;
            double edge = 0, inner = 0;
            for (uint32_t i = 0; i < n; i++)
            {
                auto at = [&](uint32_t k) { return alongX ? p.At(k, i) : p.At(i, k); };
                edge += Difference(at(0), at(last));
                inner += 0.5 * (Difference(at(0), at(1)) + Difference(at(last - 1), at(last)));
            }
            return edge <= 2.0 * inner + 0.004 * n;
        }

        // DXT stores 4x4 blocks apart: where two flat neighbours meet at a block seam with a small step, the step is the
        // compression's, not the picture's; it is spread over the four texels around the seam. A large step or a busy
        // neighbourhood is an edge of the picture and stays.
        void Deblock(Plane& p)
        {
            auto filter = [](float* p1, float* p0, float* q0, float* q1) {
                float step = 0, inside = 0;
                for (int c = 0; c < 4; c++)
                {
                    step = std::max(step, std::fabs(q0[c] - p0[c]));
                    inside = std::max(inside, std::max(std::fabs(p1[c] - p0[c]), std::fabs(q1[c] - q0[c])));
                }
                if (step == 0 || step > 0.09f || inside > 0.035f)
                    return;
                for (int c = 0; c < 4; c++)
                {
                    float d = q0[c] - p0[c];
                    p0[c] += d * 0.25f;
                    q0[c] -= d * 0.25f;
                    p1[c] += d * 0.125f;
                    q1[c] -= d * 0.125f;
                }
            };
            for (uint32_t y = 0; y < p.height; y++)
                for (uint32_t x = 4; x + 1 < p.width; x += 4)
                    filter(p.At(x - 2, y), p.At(x - 1, y), p.At(x, y), p.At(x + 1, y));
            for (uint32_t y = 4; y + 1 < p.height; y += 4)
                for (uint32_t x = 0; x < p.width; x++)
                    filter(p.At(x, y - 2), p.At(x, y - 1), p.At(x, y), p.At(x, y + 1));
        }

        // Catmull-Rom weights of the four taps around a point `t` past the second one.
        std::array<float, 4> CubicWeights(float t)
        {
            float t2 = t * t, t3 = t2 * t;
            return { (-t + 2 * t2 - t3) * 0.5f, (2 - 5 * t2 + 3 * t3) * 0.5f, (t + 4 * t2 - 3 * t3) * 0.5f,
                (-t2 + t3) * 0.5f };
        }

        // One axis doubled with Catmull-Rom, each result clamped between its two nearest inputs (no ringing). Output
        // texel 2x sits at input x - 1/4, 2x + 1 at x + 1/4: the picture does not move.
        Plane DoubleAxis(const Plane& in, bool alongX, bool wrap)
        {
            Plane out(alongX ? in.width * 2 : in.width, alongX ? in.height : in.height * 2);
            const std::array<float, 4> even = CubicWeights(0.75f), odd = CubicWeights(0.25f);
            int n = int(alongX ? in.width : in.height);
            for (uint32_t y = 0; y < out.height; y++)
                for (uint32_t x = 0; x < out.width; x++)
                {
                    uint32_t o = alongX ? x : y;
                    int base = int(o / 2) - ((o & 1) ? 1 : 2);
                    const std::array<float, 4>& w = (o & 1) ? odd : even;
                    const float* taps[4];
                    for (int k = 0; k < 4; k++)
                    {
                        int s = Address(base + k, n, wrap);
                        taps[k] = alongX ? in.At(uint32_t(s), y) : in.At(x, uint32_t(s));
                    }
                    float* t = out.At(x, y);
                    for (int c = 0; c < 4; c++)
                    {
                        float v = w[0] * taps[0][c] + w[1] * taps[1][c] + w[2] * taps[2][c] + w[3] * taps[3][c];
                        t[c] = std::clamp(v, std::min(taps[1][c], taps[2][c]), std::max(taps[1][c], taps[2][c]));
                    }
                }
            return out;
        }

        void Sample(const Plane& p, float fx, float fy, bool wrapX, bool wrapY, float out[4])
        {
            int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
            float tx = fx - float(x0), ty = fy - float(y0);
            int xa = Address(x0, int(p.width), wrapX), xb = Address(x0 + 1, int(p.width), wrapX);
            int ya = Address(y0, int(p.height), wrapY), yb = Address(y0 + 1, int(p.height), wrapY);
            const float *a = p.At(uint32_t(xa), uint32_t(ya)), *b = p.At(uint32_t(xb), uint32_t(ya));
            const float *c = p.At(uint32_t(xa), uint32_t(yb)), *d = p.At(uint32_t(xb), uint32_t(yb));
            for (int k = 0; k < 4; k++)
                out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
        }

        // Along a clear edge of the source, the doubled picture is smoothed along the edge only: the staircase of
        // the source's texels goes, the edge stays as sharp across. The edge's direction and clarity come from the
        // structure tensor of the source (Scharr gradients of luminance and alpha, summed over 3x3).
        void SmoothAlongEdges(const Plane& source, Plane& doubled, bool wrapX, bool wrapY)
        {
            uint32_t w = source.width, h = source.height;
            std::vector<float> luma(size_t(w) * h);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    const float* t = source.At(x, y);
                    luma[size_t(y) * w + x] = 0.3f * t[0] + 0.59f * t[1] + 0.11f * t[2] + 0.5f * t[3];
                }
            auto L = [&](int x, int y) {
                return luma[size_t(Address(y, int(h), wrapY)) * w + size_t(Address(x, int(w), wrapX))];
            };
            std::vector<std::array<float, 3>> tensor(size_t(w) * h);
            for (int y = 0; y < int(h); y++)
                for (int x = 0; x < int(w); x++)
                {
                    float gx = (3 * (L(x + 1, y - 1) - L(x - 1, y - 1)) + 10 * (L(x + 1, y) - L(x - 1, y)) +
                                   3 * (L(x + 1, y + 1) - L(x - 1, y + 1))) /
                               32.0f;
                    float gy = (3 * (L(x - 1, y + 1) - L(x - 1, y - 1)) + 10 * (L(x, y + 1) - L(x, y - 1)) +
                                   3 * (L(x + 1, y + 1) - L(x + 1, y - 1))) /
                               32.0f;
                    tensor[size_t(y) * w + size_t(x)] = { gx * gx, gx * gy, gy * gy };
                }
            struct Edge
            {
                float ex, ey, weight;
            };
            std::vector<Edge> edges(size_t(w) * h);
            for (int y = 0; y < int(h); y++)
                for (int x = 0; x < int(w); x++)
                {
                    float jxx = 0, jxy = 0, jyy = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            const auto& j = tensor[size_t(Address(y + dy, int(h), wrapY)) * w +
                                                   size_t(Address(x + dx, int(w), wrapX))];
                            jxx += j[0];
                            jxy += j[1];
                            jyy += j[2];
                        }
                    float half = 0.5f * (jxx - jyy), root = std::sqrt(half * half + jxy * jxy);
                    float l1 = 0.5f * (jxx + jyy) + root, l2 = 0.5f * (jxx + jyy) - root;
                    float coherence = (l1 - l2) / (l1 + l2 + 1e-9f);
                    float strength = std::sqrt(std::max(l1, 0.0f) / 9.0f);
                    float ramp = std::clamp((strength - 0.01f) / 0.04f, 0.0f, 1.0f);
                    float angle = 0.5f * std::atan2(2 * jxy, jxx - jyy); // the gradient's direction
                    edges[size_t(y) * w + size_t(x)] = { -std::sin(angle), std::cos(angle),
                        coherence * std::sqrt(coherence) * ramp * 0.9f };
                }
            Plane out = doubled;
            for (uint32_t y = 0; y < doubled.height; y++)
                for (uint32_t x = 0; x < doubled.width; x++)
                {
                    const Edge& e = edges[size_t(y / 2) * w + x / 2];
                    if (e.weight < 0.05f)
                        continue;
                    float sum[4] = {}, s[4];
                    static constexpr float Taps[7][2] = { { -3, 0.05f }, { -2, 0.1f }, { -1, 0.2f }, { 0, 0.3f },
                        { 1, 0.2f }, { 2, 0.1f }, { 3, 0.05f } };
                    for (const auto& tap : Taps)
                    {
                        Sample(doubled, float(x) + tap[0] * e.ex, float(y) + tap[0] * e.ey, wrapX, wrapY, s);
                        for (int c = 0; c < 4; c++)
                            sum[c] += tap[1] * s[c];
                    }
                    float* t = out.At(x, y);
                    const float* u = doubled.At(x, y);
                    for (int c = 0; c < 4; c++)
                        t[c] = u[c] + (sum[c] - u[c]) * e.weight;
                }
            doubled = std::move(out);
        }

        // [1 2 1] blur along both axes.
        std::vector<float> Blur(const std::vector<float>& in, int w, int h, bool wrapX, bool wrapY)
        {
            std::vector<float> across(in.size()), out(in.size());
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    across[size_t(y) * w + x] =
                        0.25f * (in[size_t(y) * w + Address(x - 1, w, wrapX)] + 2 * in[size_t(y) * w + x] +
                                    in[size_t(y) * w + Address(x + 1, w, wrapX)]);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    out[size_t(y) * w + x] =
                        0.25f * (across[size_t(Address(y - 1, h, wrapY)) * w + x] + 2 * across[size_t(y) * w + x] +
                                    across[size_t(Address(y + 1, h, wrapY)) * w + x]);
            return out;
        }

        // Warp sharpening: a doubled edge is a soft ramp two or three texels wide; each texel near a clear edge takes
        // its value from a little further from the edge (down the slope of the blurred edge map), so the ramp narrows
        // without any halo or new value (the result is a resampling of the picture).
        void WarpSharpen(Plane& p, float depth, bool wrapX, bool wrapY)
        {
            int w = int(p.width), h = int(p.height);
            std::vector<float> luma(size_t(w) * h), edge(size_t(w) * h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    const float* t = p.At(uint32_t(x), uint32_t(y));
                    luma[size_t(y) * w + x] = 0.3f * t[0] + 0.59f * t[1] + 0.11f * t[2] + 0.5f * t[3];
                }
            auto at = [&](const std::vector<float>& m, int x, int y) {
                return m[size_t(Address(y, h, wrapY)) * w + size_t(Address(x, w, wrapX))];
            };
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    float gx = 0.5f * (at(luma, x + 1, y) - at(luma, x - 1, y));
                    float gy = 0.5f * (at(luma, x, y + 1) - at(luma, x, y - 1));
                    edge[size_t(y) * w + x] = std::sqrt(gx * gx + gy * gy);
                }
            edge = Blur(Blur(edge, w, h, wrapX, wrapY), w, h, wrapX, wrapY);
            Plane out = p;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    float peak = 0;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++)
                            peak = std::max(peak, at(edge, x + dx, y + dy));
                    float gate = std::clamp((peak - 0.01f) / 0.03f, 0.0f, 1.0f);
                    if (gate <= 0)
                        continue;
                    float dx = 0.5f * (at(edge, x + 1, y) - at(edge, x - 1, y));
                    float dy = 0.5f * (at(edge, x, y + 1) - at(edge, x, y - 1));
                    float scale = depth * gate / (peak + 1e-6f);
                    float mx = std::clamp(-dx * scale, -1.0f, 1.0f), my = std::clamp(-dy * scale, -1.0f, 1.0f);
                    Sample(p, float(x) + mx, float(y) + my, wrapX, wrapY, out.At(uint32_t(x), uint32_t(y)));
                }
            p = std::move(out);
        }

        // Unsharp mask (a [1 2 1] blur), each result clamped to the 3x3 range around it: no halo, no overshoot.
        void Sharpen(Plane& p, float amount, bool wrapX, bool wrapY)
        {
            if (amount <= 0)
                return;
            int w = int(p.width), h = int(p.height);
            Plane out(p.width, p.height);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    float blur[4] = {}, lo[4], hi[4];
                    for (int c = 0; c < 4; c++)
                    {
                        lo[c] = 1e9f;
                        hi[c] = -1e9f;
                    }
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            const float* t =
                                p.At(uint32_t(Address(x + dx, w, wrapX)), uint32_t(Address(y + dy, h, wrapY)));
                            float k = float((dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1)) / 16.0f;
                            for (int c = 0; c < 4; c++)
                            {
                                blur[c] += k * t[c];
                                lo[c] = std::min(lo[c], t[c]);
                                hi[c] = std::max(hi[c], t[c]);
                            }
                        }
                    const float* t = p.At(uint32_t(x), uint32_t(y));
                    float* o = out.At(uint32_t(x), uint32_t(y));
                    for (int c = 0; c < 4; c++)
                        o[c] = std::clamp(t[c] + amount * (t[c] - blur[c]), lo[c], hi[c]);
                }
            p = std::move(out);
        }

        // ---- BC1 / BC3 ----

        constexpr float Weight[3] = { 1.0f, 1.6f, 0.6f }; // error weights of R, G, B

        uint16_t Pack565(const float c[3])
        {
            auto q = [](float v, int max) { return uint16_t(std::lround(std::clamp(v, 0.0f, 1.0f) * float(max))); };
            return uint16_t(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
        }

        void Unpack565(uint16_t v, float c[3])
        {
            uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            c[0] = float((r << 3) | (r >> 2)) / 255.0f;
            c[1] = float((g << 2) | (g >> 4)) / 255.0f;
            c[2] = float((b << 3) | (b >> 2)) / 255.0f;
        }

        struct ColorBlock
        {
            uint16_t c0 = 0, c1 = 0;
            uint32_t indices = 0;
            float error = 1e30f;
            bool three = false; // 3-colour mode (c0 <= c1)
        };

        // Indices and error for two quantized endpoints. `four`: the block is decoded in 4-colour mode whatever the
        // order (BC3); else the order chooses the mode, and `three` is required when a texel is transparent.
        ColorBlock Fit(uint16_t q0, uint16_t q1, const float rgb[16][3], const bool transparent[16],
            bool anyTransparent, bool four)
        {
            ColorBlock block;
            if (four)
            {
                if (q0 < q1)
                    std::swap(q0, q1);
            }
            else if (anyTransparent || q0 == q1)
            {
                if (q0 > q1)
                    std::swap(q0, q1);
                block.three = true;
            }
            else if (q0 < q1)
                std::swap(q0, q1);
            block.c0 = q0;
            block.c1 = q1;
            float palette[4][3];
            Unpack565(q0, palette[0]);
            Unpack565(q1, palette[1]);
            int colors = block.three ? 3 : 4;
            for (int c = 0; c < 3; c++)
            {
                if (block.three)
                    palette[2][c] = 0.5f * (palette[0][c] + palette[1][c]);
                else
                {
                    palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3.0f;
                    palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3.0f;
                }
            }
            block.error = 0;
            for (int i = 0; i < 16; i++)
            {
                if (transparent && transparent[i])
                {
                    block.indices |= 3u << (2 * i);
                    continue;
                }
                int best = 0;
                float bestError = 1e30f;
                for (int k = 0; k < colors; k++)
                {
                    float e = 0;
                    for (int c = 0; c < 3; c++)
                    {
                        float d = rgb[i][c] - palette[k][c];
                        e += Weight[c] * d * d;
                    }
                    if (e < bestError)
                    {
                        bestError = e;
                        best = k;
                    }
                }
                block.indices |= uint32_t(best) << (2 * i);
                block.error += bestError;
            }
            return block;
        }

        // The endpoints that fit the block's current indices best (least squares), then fitted again.
        bool Refine(
            const ColorBlock& block, const float rgb[16][3], const bool transparent[16], float e0[3], float e1[3])
        {
            float aa = 0, bb = 0, ab = 0, ax[3] = {}, bx[3] = {};
            for (int i = 0; i < 16; i++)
            {
                uint32_t index = (block.indices >> (2 * i)) & 3;
                if ((transparent && transparent[i]) || (block.three && index == 3))
                    continue;
                float a = index == 0 ? 1.0f : index == 1 ? 0.0f : block.three ? 0.5f : index == 2 ? 2 / 3.0f : 1 / 3.0f;
                float b = 1 - a;
                aa += a * a;
                bb += b * b;
                ab += a * b;
                for (int c = 0; c < 3; c++)
                {
                    ax[c] += a * rgb[i][c];
                    bx[c] += b * rgb[i][c];
                }
            }
            float det = aa * bb - ab * ab;
            if (std::fabs(det) < 1e-6f)
                return false;
            for (int c = 0; c < 3; c++)
            {
                e0[c] = std::clamp((bb * ax[c] - ab * bx[c]) / det, 0.0f, 1.0f);
                e1[c] = std::clamp((aa * bx[c] - ab * ax[c]) / det, 0.0f, 1.0f);
            }
            return true;
        }

        void EncodeColor(const float rgb[16][3], const bool transparent[16], bool four, uint8_t out[8])
        {
            bool anyTransparent = false;
            int count = 0;
            float mean[3] = {};
            for (int i = 0; i < 16; i++)
            {
                if (transparent && transparent[i])
                {
                    anyTransparent = true;
                    continue;
                }
                count++;
                for (int c = 0; c < 3; c++)
                    mean[c] += rgb[i][c];
            }
            ColorBlock best;
            if (count == 0)
            {
                best.c0 = best.c1 = 0;
                best.indices = 0xFFFFFFFFu;
            }
            else
            {
                for (float& m : mean)
                    m /= float(count);
                float cov[6] = {}; // xx xy xz yy yz zz
                for (int i = 0; i < 16; i++)
                {
                    if (transparent && transparent[i])
                        continue;
                    float d[3] = { rgb[i][0] - mean[0], rgb[i][1] - mean[1], rgb[i][2] - mean[2] };
                    cov[0] += d[0] * d[0];
                    cov[1] += d[0] * d[1];
                    cov[2] += d[0] * d[2];
                    cov[3] += d[1] * d[1];
                    cov[4] += d[1] * d[2];
                    cov[5] += d[2] * d[2];
                }
                float axis[3] = { 1, 1, 1 };
                for (int iteration = 0; iteration < 8; iteration++)
                {
                    float v[3] = { cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                        cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                        cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
                    float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (length < 1e-12f)
                        break;
                    for (int c = 0; c < 3; c++)
                        axis[c] = v[c] / length;
                }
                float lo = 1e9f, hi = -1e9f;
                for (int i = 0; i < 16; i++)
                {
                    if (transparent && transparent[i])
                        continue;
                    float t = (rgb[i][0] - mean[0]) * axis[0] + (rgb[i][1] - mean[1]) * axis[1] +
                              (rgb[i][2] - mean[2]) * axis[2];
                    lo = std::min(lo, t);
                    hi = std::max(hi, t);
                }
                float e0[3], e1[3];
                for (int c = 0; c < 3; c++)
                {
                    e0[c] = mean[c] + axis[c] * hi;
                    e1[c] = mean[c] + axis[c] * lo;
                }
                best = Fit(Pack565(e0), Pack565(e1), rgb, transparent, anyTransparent, four);
                for (int iteration = 0; iteration < 2 && best.error > 0; iteration++)
                {
                    if (!Refine(best, rgb, transparent, e0, e1))
                        break;
                    ColorBlock next = Fit(Pack565(e0), Pack565(e1), rgb, transparent, anyTransparent, four);
                    if (next.error >= best.error)
                        break;
                    best = next;
                }
            }
            out[0] = uint8_t(best.c0);
            out[1] = uint8_t(best.c0 >> 8);
            out[2] = uint8_t(best.c1);
            out[3] = uint8_t(best.c1 >> 8);
            for (int k = 0; k < 4; k++)
                out[4 + k] = uint8_t(best.indices >> (8 * k));
        }

        // A BC4 block (BC3's alpha): the better of the 8-value mode (a0 > a1) and the 6-value one with 0 and 255.
        void EncodeAlpha(const uint8_t a[16], uint8_t out[8])
        {
            auto fit = [&](int a0, int a1, uint64_t& bits) {
                int palette[8] = { a0, a1 };
                if (a0 > a1)
                    for (int i = 1; i <= 6; i++)
                        palette[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
                else
                {
                    for (int i = 1; i <= 4; i++)
                        palette[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
                    palette[6] = 0;
                    palette[7] = 255;
                }
                int error = 0;
                bits = 0;
                for (int i = 0; i < 16; i++)
                {
                    int best = 0, bestError = 1 << 30;
                    for (int k = 0; k < 8; k++)
                    {
                        int d = int(a[i]) - palette[k];
                        if (d * d < bestError)
                        {
                            bestError = d * d;
                            best = k;
                        }
                    }
                    bits |= uint64_t(best) << (3 * i);
                    error += bestError;
                }
                return error;
            };
            int lo = 255, hi = 0, innerLo = 255, innerHi = 0;
            for (int i = 0; i < 16; i++)
            {
                lo = std::min(lo, int(a[i]));
                hi = std::max(hi, int(a[i]));
                if (a[i] != 0 && a[i] != 255)
                {
                    innerLo = std::min(innerLo, int(a[i]));
                    innerHi = std::max(innerHi, int(a[i]));
                }
            }
            uint64_t bits8 = 0, bits6 = 0;
            int a0 = hi, a1 = lo;
            int error8 = hi > lo ? fit(hi, lo, bits8) : 1 << 30;
            if (innerLo > innerHi)
                innerLo = innerHi = lo == 255 ? 255 : 0;
            int error6 = fit(innerLo, innerHi, bits6);
            uint64_t bits = bits8;
            if (error6 < error8)
            {
                a0 = innerLo;
                a1 = innerHi;
                bits = bits6;
            }
            out[0] = uint8_t(a0);
            out[1] = uint8_t(a1);
            for (int k = 0; k < 6; k++)
                out[2 + k] = uint8_t(bits >> (8 * k));
        }

        void ReadBlock(const art::Image& image, uint32_t bx, uint32_t by, uint32_t texels[16])
        {
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++)
                {
                    uint32_t sx = std::min(bx * 4 + x, image.width - 1), sy = std::min(by * 4 + y, image.height - 1);
                    texels[y * 4 + x] = image.rgba[size_t(sy) * image.width + sx];
                }
        }

        template <typename Encode>
        std::vector<uint8_t> EncodeBlocks(const art::Image& image, size_t blockBytes, Encode encode)
        {
            uint32_t bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
            std::vector<uint8_t> out(size_t(bw) * bh * blockBytes);
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++)
                {
                    uint32_t texels[16];
                    ReadBlock(image, bx, by, texels);
                    encode(texels, &out[(size_t(by) * bw + bx) * blockBytes]);
                }
            return out;
        }

        void ToRgb(const uint32_t texels[16], float rgb[16][3])
        {
            for (int i = 0; i < 16; i++)
                for (int c = 0; c < 3; c++)
                    rgb[i][c] = Channel(texels[i], c);
        }
    }

    const char* KindName(Kind kind)
    {
        switch (kind)
        {
        case Kind::Color: return "color";
        case Kind::Tiny: return "tiny";
        case Kind::Flat: return "flat";
        case Kind::Normal: return "normal";
        case Kind::Mask: return "mask";
        }
        return "?";
    }

    Analysis Analyse(const art::Image& image)
    {
        Analysis analysis;
        uint32_t w = image.width, h = image.height;
        if (std::min(w, h) < 32 || uint64_t(w) * h < 64 * 64)
        {
            analysis.kind = Kind::Tiny;
            return analysis;
        }
        double n = double(image.rgba.size());
        double sum[4] = {}, squares[4] = {};
        size_t translucent = 0, middle = 0, grey = 0, unit = 0;
        for (uint32_t t : image.rgba)
        {
            int v[4];
            for (int c = 0; c < 4; c++)
            {
                v[c] = int((t >> (8 * c)) & 0xFF);
                sum[c] += v[c];
                squares[c] += double(v[c]) * v[c];
            }
            translucent += v[3] < 250;
            middle += v[3] >= 8 && v[3] <= 247;
            grey += std::abs(v[0] - v[1]) <= 3 && std::abs(v[1] - v[2]) <= 3;
            float x = v[0] / 127.5f - 1, y = v[1] / 127.5f - 1, z = v[2] / 127.5f - 1;
            unit += z >= 0 && std::fabs(x * x + y * y + z * z - 1) < 0.2f;
        }
        double deviation[4], mean[4];
        for (int c = 0; c < 4; c++)
        {
            mean[c] = sum[c] / n;
            deviation[c] = std::sqrt(std::max(0.0, squares[c] / n - mean[c] * mean[c]));
        }
        analysis.alpha = translucent == 0 ? Alpha::Opaque : double(middle) <= 0.002 * n ? Alpha::Cutout : Alpha::Blend;
        double most = std::max({ deviation[0], deviation[1], deviation[2], deviation[3] });
        bool colorFlat = std::max({ deviation[0], deviation[1], deviation[2] }) < 3.0;
        if (most < 1.0)
            analysis.kind = Kind::Flat;
        else if ((unit >= 0.95 * n && mean[2] > 190) ||
                 (deviation[0] < 2.0 && deviation[2] < 2.0 && deviation[1] > 4.0 && deviation[3] > 4.0))
            analysis.kind = Kind::Normal;
        else if ((analysis.alpha == Alpha::Opaque && grey >= 0.99 * n) || (colorFlat && deviation[3] > 4.0))
            analysis.kind = Kind::Mask;
        return analysis;
    }

    art::Image Upscale2x(const art::Image& image, Alpha alpha, const UpscaleOptions& options)
    {
        Plane source = Premultiplied(image, alpha == Alpha::Opaque);
        if (options.deblock)
            Deblock(source);
        bool wrapX = Tiles(source, true), wrapY = Tiles(source, false);
        Plane doubled = DoubleAxis(DoubleAxis(source, true, wrapX), false, wrapY);
        SmoothAlongEdges(source, doubled, wrapX, wrapY);
        WarpSharpen(doubled, options.warp, wrapX, wrapY);
        Sharpen(doubled, options.sharpen, wrapX, wrapY);

        art::Image out;
        out.width = doubled.width;
        out.height = doubled.height;
        out.rgba.resize(size_t(out.width) * out.height);
        for (uint32_t y = 0; y < out.height; y++)
            for (uint32_t x = 0; x < out.width; x++)
            {
                float* t = doubled.At(x, y);
                float a = std::clamp(t[3], 0.0f, 1.0f);
                uint32_t rgb = 0;
                if (alpha == Alpha::Opaque)
                    a = 1;
                if (a > 1.0f / 255.0f)
                {
                    for (int c = 0; c < 3; c++)
                        rgb |= Byte(t[c] / a) << (8 * c);
                }
                else // under nothing: the colour of the source texel, so filtering at a distance shows no black edge
                    rgb = image.rgba[size_t(y / 2) * image.width + x / 2] & 0x00FFFFFFu;
                if (alpha == Alpha::Cutout)
                    a = a >= 0.5f ? 1.0f : 0.0f;
                out.rgba[size_t(y) * out.width + x] = rgb | Byte(a) << 24;
            }
        return out;
    }

    std::vector<art::Image> MipChain(const art::Image& top)
    {
        std::vector<art::Image> levels;
        const art::Image* above = &top;
        while (above->width > 1 || above->height > 1)
        {
            art::Image level;
            level.width = std::max(1u, above->width / 2);
            level.height = std::max(1u, above->height / 2);
            level.rgba.resize(size_t(level.width) * level.height);
            for (uint32_t y = 0; y < level.height; y++)
                for (uint32_t x = 0; x < level.width; x++)
                {
                    float premultiplied[3] = {}, straight[3] = {}, alpha = 0;
                    for (uint32_t k = 0; k < 4; k++)
                    {
                        uint32_t sx = std::min(x * 2 + (k & 1), above->width - 1);
                        uint32_t sy = std::min(y * 2 + (k >> 1), above->height - 1);
                        uint32_t t = above->rgba[size_t(sy) * above->width + sx];
                        float a = Channel(t, 3);
                        alpha += a;
                        for (int c = 0; c < 3; c++)
                        {
                            premultiplied[c] += Channel(t, c) * a;
                            straight[c] += Channel(t, c);
                        }
                    }
                    uint32_t texel = Byte(alpha / 4) << 24;
                    for (int c = 0; c < 3; c++)
                        texel |= Byte(alpha > 0 ? premultiplied[c] / alpha : straight[c] / 4) << (8 * c);
                    level.rgba[size_t(y) * level.width + x] = texel;
                }
            levels.push_back(std::move(level));
            above = &levels.back();
        }
        return levels;
    }

    std::vector<uint8_t> EncodeBc1(const art::Image& image, bool alpha)
    {
        return EncodeBlocks(image, 8, [alpha](const uint32_t texels[16], uint8_t* out) {
            float rgb[16][3];
            bool transparent[16];
            ToRgb(texels, rgb);
            for (int i = 0; i < 16; i++)
                transparent[i] = alpha && (texels[i] >> 24) < 128;
            EncodeColor(rgb, transparent, false, out);
        });
    }

    std::vector<uint8_t> EncodeBc3(const art::Image& image)
    {
        return EncodeBlocks(image, 16, [](const uint32_t texels[16], uint8_t* out) {
            float rgb[16][3];
            uint8_t a[16];
            ToRgb(texels, rgb);
            for (int i = 0; i < 16; i++)
                a[i] = uint8_t(texels[i] >> 24);
            EncodeAlpha(a, out);
            EncodeColor(rgb, nullptr, true, out + 8);
        });
    }
}
