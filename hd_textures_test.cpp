// ArkeyanHdTexturesTest: the HD texture pass (hd_upscale.h, hd_textures.h) on synthetic pictures: what is upscaled, the 2x
// (size, no ringing, cut-outs stay crisp, no dark fringe), the mip chain, BC1/BC3 round trips, the pack file and the
// [Lux] disc reader (a synthetic APKF stream). No game data. Argument: a scratch folder.
#include "hd_textures.h"
#include "host/dds.h"
#include "host/texture_pack.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
    int failures = 0;

    uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | g << 8 | b << 16 | a << 24; }
    uint32_t C(uint32_t texel, int channel) { return (texel >> (8 * channel)) & 0xFF; }

    art::Image Make(uint32_t w, uint32_t h, uint32_t (*texel)(uint32_t x, uint32_t y))
    {
        art::Image image;
        image.width = w;
        image.height = h;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                image.rgba.push_back(texel(x, y));
        return image;
    }

    // A deterministic, colourful picture: gradients and a few edges.
    uint32_t Picture(uint32_t x, uint32_t y)
    {
        uint32_t r = (x * 4) & 0xFF, g = (y * 3 + 40) & 0xFF, b = ((x ^ y) & 16) ? 200 : 60;
        return Rgba(r, g, b);
    }

    art::Image Decode(const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h, art::TextureFormat format)
    {
        hdtex::Source source;
        source.format = format;
        source.width = w;
        source.height = h;
        source.data = blocks;
        return hdtex::Decode(source);
    }

    double MaxError(const art::Image& a, const art::Image& b, int channels)
    {
        double worst = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
            for (int c = 0; c < channels; c++)
                worst = std::max(worst, std::fabs(double(C(a.rgba[i], c)) - double(C(b.rgba[i], c))));
        return worst;
    }

    double MeanError(const art::Image& a, const art::Image& b)
    {
        double sum = 0;
        for (size_t i = 0; i < a.rgba.size(); i++)
            for (int c = 0; c < 3; c++)
                sum += std::fabs(double(C(a.rgba[i], c)) - double(C(b.rgba[i], c)));
        return sum / double(a.rgba.size() * 3);
    }

    void TestAnalyse()
    {
        using hdtex::Kind;
        CHECK(hdtex::Analyse(Make(16, 64, Picture)).kind == Kind::Tiny);
        CHECK(hdtex::Analyse(Make(64, 64, [](uint32_t, uint32_t) { return Rgba(10, 20, 30); })).kind == Kind::Flat);
        hdtex::Analysis color = hdtex::Analyse(Make(64, 64, Picture));
        CHECK(color.kind == Kind::Color && color.alpha == hdtex::Alpha::Opaque);
        // Normal map: unit vectors around +Z.
        auto normal = hdtex::Analyse(Make(64, 64, [](uint32_t x, uint32_t y) {
            float nx = std::sin(float(x) * 0.3f) * 0.3f, ny = std::cos(float(y) * 0.2f) * 0.3f;
            float nz = std::sqrt(1 - nx * nx - ny * ny);
            return Rgba(uint32_t((nx + 1) * 127.5f), uint32_t((ny + 1) * 127.5f), uint32_t((nz + 1) * 127.5f));
        }));
        CHECK(normal.kind == Kind::Normal);
        // Swizzled normal map (X in alpha, Y in green).
        CHECK(hdtex::Analyse(Make(64, 64, [](uint32_t x, uint32_t y) {
            return Rgba(255, (y * 4) & 0xFF, 0, (x * 4) & 0xFF);
        })).kind == Kind::Normal);
        CHECK(hdtex::Analyse(Make(64, 64, [](uint32_t x, uint32_t y) {
            uint32_t v = (x * 3 + y) & 0xFF;
            return Rgba(v, v, v);
        })).kind == Kind::Mask);
        // A font: white, the glyphs in alpha.
        CHECK(hdtex::Analyse(Make(64, 64, [](uint32_t x, uint32_t y) {
            return Rgba(255, 255, 255, ((x / 3 + y / 5) & 1) ? 255 : (x * 7) & 0xFF);
        })).kind == Kind::Mask);
        auto cutout = hdtex::Analyse(Make(64, 64, [](uint32_t x, uint32_t y) {
            return Picture(x, y) & (((x - 32) * (x - 32) + (y - 32) * (y - 32) < 400) ? 0xFFFFFFFFu : 0x00FFFFFFu);
        }));
        CHECK(cutout.kind == Kind::Color && cutout.alpha == hdtex::Alpha::Cutout);
        auto blend = hdtex::Analyse(Make(
            64, 64, [](uint32_t x, uint32_t y) { return (Picture(x, y) & 0x00FFFFFFu) | ((x * 4) & 0xFF) << 24; }));
        CHECK(blend.kind == Kind::Color && blend.alpha == hdtex::Alpha::Blend);
    }

    void TestUpscale()
    {
        // A sharp vertical edge, dark | light: the halves stay exact, the edge is monotonic, nothing overshoots.
        art::Image edge =
            Make(64, 64, [](uint32_t x, uint32_t) { return x < 31 ? Rgba(20, 30, 40) : Rgba(220, 210, 200); });
        art::Image up = hdtex::Upscale2x(edge, hdtex::Alpha::Opaque, hdtex::UpscaleOptions{ .deblock = false });
        CHECK(up.width == 128 && up.height == 128);
        bool exact = true, monotonic = true, bounded = true;
        for (uint32_t y = 0; y < 128; y++)
        {
            for (uint32_t x = 0; x < 128; x++)
            {
                uint32_t t = up.rgba[y * 128 + x];
                bounded = bounded && C(t, 0) >= 20 && C(t, 0) <= 220 && C(t, 3) == 255;
                if (x > 0)
                    monotonic = monotonic && C(t, 0) >= C(up.rgba[y * 128 + x - 1], 0);
            }
            exact = exact && up.rgba[y * 128 + 10] == Rgba(20, 30, 40) && up.rgba[y * 128 + 100] == Rgba(220, 210, 200);
        }
        CHECK(exact);
        CHECK(monotonic);
        CHECK(bounded);

        // A red disc on transparent: alpha stays 0 or 255, about four times the coverage, no dark fringe.
        art::Image disc = Make(64, 64, [](uint32_t x, uint32_t y) {
            bool in = (x - 32) * (x - 32) + (y - 32) * (y - 32) < 300;
            return Rgba(230, 40, 30, in ? 255 : 0);
        });
        art::Image cut = hdtex::Upscale2x(disc, hdtex::Alpha::Cutout);
        size_t before = 0, after = 0;
        bool binary = true, red = true;
        for (uint32_t t : disc.rgba)
            before += C(t, 3) == 255;
        for (uint32_t t : cut.rgba)
        {
            binary = binary && (C(t, 3) == 0 || C(t, 3) == 255);
            after += C(t, 3) == 255;
            red = red && C(t, 0) >= 200 && C(t, 1) <= 60;
        }
        CHECK(binary);
        CHECK(red);
        CHECK(std::fabs(double(after) / double(4 * before) - 1) < 0.03);
        // The same graded (blend): the colour under any alpha stays the disc's red.
        art::Image soft = hdtex::Upscale2x(disc, hdtex::Alpha::Blend);
        red = true;
        for (uint32_t t : soft.rgba)
            red = red && (C(t, 3) == 0 || (C(t, 0) >= 200 && C(t, 1) <= 60));
        CHECK(red);

        // A tiling texture joins across its edges: the first and the last column of the result stay close.
        art::Image tile = Make(64, 64, [](uint32_t x, uint32_t y) {
            uint32_t v =
                uint32_t(127 + 100 * std::sin(float(x) * 6.2831853f / 64) * std::cos(float(y) * 6.2831853f / 32));
            return Rgba(v, 255 - v, 128);
        });
        art::Image tiled = hdtex::Upscale2x(tile, hdtex::Alpha::Opaque);
        double seam = 0;
        for (uint32_t y = 0; y < 128; y++)
            seam =
                std::max(seam, std::fabs(double(C(tiled.rgba[y * 128], 0)) - double(C(tiled.rgba[y * 128 + 127], 0))));
        CHECK(seam < 24);
    }

    void TestMips()
    {
        art::Image top = Make(64, 32, Picture);
        std::vector<art::Image> mips = hdtex::MipChain(top);
        CHECK(mips.size() == 6);
        CHECK(mips.back().width == 1 && mips.back().height == 1);
        CHECK(mips[0].width == 32 && mips[0].height == 16);
        // A half-transparent level keeps the colour of what is opaque (premultiplied): no dark fringe.
        art::Image half =
            Make(2, 2, [](uint32_t x, uint32_t) { return x == 0 ? Rgba(200, 100, 50) : Rgba(0, 0, 0, 0); });
        std::vector<art::Image> level = hdtex::MipChain(half);
        CHECK(level.size() == 1 && C(level[0].rgba[0], 0) == 200 && C(level[0].rgba[0], 3) == 128);
    }

    void TestEncoders()
    {
        art::Image picture = Make(64, 64, Picture);
        art::Image bc1 = Decode(hdtex::EncodeBc1(picture, false), 64, 64, art::TextureFormat::Dxt1);
        CHECK(MeanError(picture, bc1) < 6);
        art::Image solid = Make(8, 8, [](uint32_t, uint32_t) { return Rgba(255, 0, 255); });
        CHECK(MaxError(solid, Decode(hdtex::EncodeBc1(solid, false), 8, 8, art::TextureFormat::Dxt1), 4) == 0);
        // BC1 with alpha: the transparent texels decode transparent, the others opaque.
        art::Image holes = Make(16, 16, [](uint32_t x, uint32_t y) { return ((x + y) % 3) ? Picture(x, y) : 0u; });
        art::Image back = Decode(hdtex::EncodeBc1(holes, true), 16, 16, art::TextureFormat::Dxt1);
        bool alpha = true;
        for (size_t i = 0; i < holes.rgba.size(); i++)
            alpha = alpha && C(back.rgba[i], 3) == C(holes.rgba[i], 3);
        CHECK(alpha);
        // BC3: a graded alpha within a few steps; odd sizes padded.
        art::Image ramp =
            Make(30, 18, [](uint32_t x, uint32_t y) { return (Picture(x, y) & 0x00FFFFFFu) | (x * 8) << 24; });
        art::Image bc3 = Decode(hdtex::EncodeBc3(ramp), 30, 18, art::TextureFormat::Dxt5);
        double worstAlpha = 0;
        for (size_t i = 0; i < ramp.rgba.size(); i++)
            worstAlpha = std::max(worstAlpha, std::fabs(double(C(ramp.rgba[i], 3)) - double(C(bc3.rgba[i], 3))));
        CHECK(worstAlpha <= 6);
        CHECK(MeanError(ramp, bc3) < 6);
    }

    void Be32(std::vector<uint8_t>& out, size_t at, uint32_t value)
    {
        for (int i = 0; i < 4; i++)
            out[at + i] = uint8_t(value >> (24 - 8 * i));
    }

    // One record of a Lux stream holding an APKF package: the texture `linear` (DXT1 blocks in the PC order, `w` x `h`)
    // stored as the console does, tiled in 32-block tiles and 8-in-16 swapped, with its fetch constant.
    std::vector<uint8_t> LuxRecord(const std::vector<uint8_t>& linear, uint32_t w, uint32_t h, const char* name)
    {
        uint32_t blocksX = w / 4, blocksY = h / 4, pitchBlocks = (blocksX + 31) / 32 * 32;
        std::vector<uint8_t> pixels(size_t(pitchBlocks) * ((blocksY + 31) / 32 * 32) * 8);
        for (uint32_t y = 0; y < blocksY; y++)
            for (uint32_t x = 0; x < blocksX; x++)
                for (uint32_t i = 0; i < 8; i++)
                    pixels[art::TiledOffset(x, y, pitchBlocks, 3) + (i ^ 1)] =
                        linear[(size_t(y) * blocksX + x) * 8 + i];
        std::vector<uint8_t> body(0x1000 + pixels.size());
        memcpy(body.data(), "APKF", 4);
        memcpy(&body[0x40], "\0XET", 4);
        strcpy(reinterpret_cast<char*>(&body[0x70]), name);
        static const uint8_t Flushes[8] = { 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
        memcpy(&body[0xC0], Flushes, 8);
        Be32(body, 0xC8, 2u | 1u << 31 | (pitchBlocks * 4 / 32) << 22); // 2D, tiled, pitch in 32 texels
        Be32(body, 0xCC, 18u | 1u << 6);                                // DXT1, 8-in-16
        Be32(body, 0xD0, (w - 1) | (h - 1) << 13);
        Be32(body, 0xDC, 1u << 9); // 2D
        memcpy(&body[0x100], "SYHP", 4);
        Be32(body, 0x108, 0x1000);
        Be32(body, 0x110, uint32_t(pixels.size()));
        memcpy(&body[0x1000], pixels.data(), pixels.size());
        std::vector<uint8_t> record(16);
        Be32(record, 0, 0xB053E597);
        Be32(record, 4, 0x12345678);
        Be32(record, 12, uint32_t(body.size()));
        record.insert(record.end(), body.begin(), body.end());
        return record;
    }

    void TestLux()
    {
        std::vector<uint8_t> a = hdtex::EncodeBc1(Make(64, 64, Picture), false);
        std::vector<uint8_t> b = hdtex::EncodeBc1(Make(160, 32, Picture), false);
        // A level's stream: a table first (not records), then a record of another kind, then the two packages.
        std::vector<uint8_t> stream = { 0x85, 0x92, 0x6D, 0x4A, 0, 0, 0, 2, 0xB0, 0x53, 0xE5, 0x97, 0, 0, 0, 2 };
        std::vector<uint8_t> other(16 + 40, 0x41);
        Be32(other, 8, 0);
        Be32(other, 12, 40);
        stream.insert(stream.end(), other.begin(), other.end());
        for (const auto& record : { LuxRecord(a, 64, 64, "rock_a"), LuxRecord(b, 160, 32, "sign_b") })
            stream.insert(stream.end(), record.begin(), record.end());
        std::vector<hdtex::Source> sources;
        hdtex::LuxPicturesOf(stream, sources);
        CHECK(sources.size() == 2);
        if (sources.size() == 2)
        {
            CHECK(sources[0].name == "rock_a" && sources[0].format == art::TextureFormat::Dxt1 &&
                  sources[0].width == 64 && sources[0].height == 64 && sources[0].data == a);
            CHECK(sources[0].hash == texpack::ContentHash(18, 64, 64, a.data(), a.size()));
            CHECK(sources[1].name == "sign_b" && sources[1].width == 160 && sources[1].data == b);
        }
        // A bare package (textures/*.apk) is one APKF body.
        std::vector<uint8_t> record = LuxRecord(a, 64, 64, "loading");
        std::vector<uint8_t> bare(record.begin() + 16, record.end());
        sources.clear();
        hdtex::LuxPicturesOf(bare, sources);
        CHECK(sources.size() == 1 && sources[0].data == a);
    }

    void TestPack(const std::filesystem::path& scratch)
    {
        std::filesystem::remove_all(scratch);
        std::filesystem::create_directories(scratch);
        hdtex::Source source;
        source.format = art::TextureFormat::Dxt5;
        source.width = 64;
        source.height = 64;
        source.data = hdtex::EncodeBc3(Make(64, 64, Picture));
        source.hash = texpack::ContentHash(20, 64, 64, source.data.data(), source.data.size());
        hdtex::Outcome outcome = hdtex::Process(source, 512, scratch);
        CHECK(outcome.written);
        std::filesystem::path file = scratch / (texpack::HashName(source.hash) + ".dds");
        std::ifstream in(file, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        dds::Info info;
        std::string error;
        CHECK(dds::Parse(bytes, bytes.size(), info, error));
        // Opaque: BC1 (half a DXT5's memory), doubled, with a full chain.
        CHECK(info.format == dds::Format::BC1 && info.width == 128 && info.height == 128 && info.levels == 8);
        CHECK(outcome.packBytes > outcome.sourceBytes && outcome.packBytes < 3 * outcome.sourceBytes);

        hdtex::Source big = source;
        big.hash++;
        CHECK(!hdtex::Process(big, 32, scratch).written && hdtex::Process(big, 32, scratch).tooLarge);

        // A dump file reads back as the same source.
        std::span<const uint8_t> level(source.data);
        std::vector<uint8_t> dumped = dds::Write(dds::Format::BC3, 64, 64, std::span(&level, 1));
        std::filesystem::path dumpFile = scratch / (texpack::HashName(source.hash) + ".dds");
        std::filesystem::path other = scratch / "dump";
        std::filesystem::create_directories(other);
        dumpFile = other / dumpFile.filename();
        std::ofstream(dumpFile, std::ios::binary)
            .write(reinterpret_cast<const char*>(dumped.data()), std::streamsize(dumped.size()));
        hdtex::Source read;
        CHECK(hdtex::ReadDumpFile(dumpFile, read));
        CHECK(read.hash == source.hash && read.width == 64 && read.height == 64 && read.data == source.data &&
              read.format == art::TextureFormat::Dxt5);

        // An uncompressed source is kept (BC would lose its precision).
        hdtex::Source rgba = source;
        rgba.hash++;
        rgba.format = art::TextureFormat::Argb8;
        art::Image picture = Make(64, 64, Picture);
        rgba.data.assign(reinterpret_cast<const uint8_t*>(picture.rgba.data()),
            reinterpret_cast<const uint8_t*>(picture.rgba.data()) + picture.rgba.size() * 4);
        CHECK(!hdtex::Process(rgba, 512, scratch).written);

        hdtex::Mode mode;
        CHECK(hdtex::ParseMode("2x", mode) && mode == hdtex::Mode::Double && !hdtex::ParseMode("4x", mode));
        CHECK(hdtex::MaxSide(hdtex::Mode::Auto, 4ull << 30) == 512 &&
              hdtex::MaxSide(hdtex::Mode::Auto, 8ull << 30) == 1024);
    }
}

int main(int argc, char** argv)
{
    std::filesystem::path scratch = argc > 1 ? argv[1] : "hd-textures-test";
    TestAnalyse();
    TestUpscale();
    TestMips();
    TestEncoders();
    TestPack(scratch);
    TestLux();
    printf(failures ? "ArkeyanHdTexturesTest: %d FAILED\n" : "ArkeyanHdTexturesTest: all passed\n", failures);
    return failures ? 1 : 0;
}
