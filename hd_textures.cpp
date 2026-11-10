// See hd_textures.h.
#include "hd_textures.h"
#include "host/dds.h"
#include "host/art/lux_images.h"
#include "host/texture_pack.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace hdtex
{
    namespace
    {
        uint32_t BlockBytes(art::TextureFormat format) { return format == art::TextureFormat::Dxt1 ? 8 : 16; }

        bool IsBlock(art::TextureFormat format)
        {
            return format == art::TextureFormat::Dxt1 || format == art::TextureFormat::Dxt3 ||
                   format == art::TextureFormat::Dxt5;
        }

        // A full chain's bytes for a picture of this format and size.
        uint64_t ChainBytes(dds::Format format, uint32_t width, uint32_t height)
        {
            uint64_t bytes = 0;
            for (uint32_t level = 0; level < dds::FullChain(width, height); level++)
                bytes += dds::SizeOfLevel(format, width, height, level).Bytes();
            return bytes;
        }

        dds::Format DdsFormat(art::TextureFormat format)
        {
            switch (format)
            {
            case art::TextureFormat::Dxt1: return dds::Format::BC1;
            case art::TextureFormat::Dxt3: return dds::Format::BC2;
            case art::TextureFormat::Dxt5: return dds::Format::BC3;
            default: return dds::Format::RGBA8;
            }
        }
    }

    bool ParseMode(std::string_view text, Mode& mode)
    {
        if (text == "off")
            mode = Mode::Off;
        else if (text == "2x")
            mode = Mode::Double;
        else if (text == "auto")
            mode = Mode::Auto;
        else
            return false;
        return true;
    }

    const char* ModeName(Mode mode)
    {
        switch (mode)
        {
        case Mode::Off: return "off";
        case Mode::Double: return "2x";
        case Mode::Auto: return "auto";
        }
        return "?";
    }

    uint32_t MaxSide(Mode mode, uint64_t physicalMemory)
    {
        switch (mode)
        {
        case Mode::Off: return 0;
        case Mode::Double: return 1024;
        case Mode::Auto: return physicalMemory >= (8ull << 30) - (512ull << 20) ? 1024 : 512;
        }
        return 0;
    }

    uint32_t XenosFormat(art::TextureFormat format)
    {
        switch (format)
        {
        case art::TextureFormat::Dxt1: return 18;
        case art::TextureFormat::Dxt3: return 19;
        case art::TextureFormat::Dxt5: return 20;
        case art::TextureFormat::Argb8: return 6;
        case art::TextureFormat::L8: return 2;
        }
        return 0;
    }

    namespace
    {
        // A DXT texture's top level from the console's storage (`pitchBlocks` blocks a row, tiled or linear, in its
        // byte order) to the renderer's upload: linear rows of blocks, byte-swapped, named by its hash. False if the
        // storage is too short.
        bool FromConsole(const uint8_t* pixels, size_t size, const art::Texture& texture, uint32_t pitchBlocks,
            std::string name, Source& source)
        {
            uint32_t blocksX = (texture.width + 3) / 4, blocksY = (texture.height + 3) / 4;
            uint32_t bytes = BlockBytes(texture.format), log2 = bytes == 8 ? 3 : 4;
            source.format = texture.format;
            source.width = texture.width;
            source.height = texture.height;
            source.name = std::move(name);
            source.data.resize(size_t(blocksX) * blocksY * bytes);
            for (uint32_t y = 0; y < blocksY; y++)
                for (uint32_t x = 0; x < blocksX; x++)
                {
                    size_t from = texture.tiled ? art::TiledOffset(x, y, pitchBlocks, log2)
                                                : (size_t(y) * pitchBlocks + x) * bytes;
                    if (from + bytes > size)
                        return false;
                    memcpy(&source.data[(size_t(y) * blocksX + x) * bytes], &pixels[from], bytes);
                }
            uint32_t swap = texture.endian == art::TextureEndian::In16   ? 1
                            : texture.endian == art::TextureEndian::In32 ? 3
                                                                         : 0;
            if (swap != 0)
                for (size_t i = 0; i + swap < source.data.size(); i += swap + 1)
                    std::reverse(&source.data[i], &source.data[i + swap + 1]);
            source.hash = texpack::ContentHash(
                XenosFormat(source.format), source.width, source.height, source.data.data(), source.data.size());
            return true;
        }

        bool Wanted(const art::Texture& texture)
        {
            // Smaller than 32 texels a side: the top level may sit in the packed mip tail.
            return IsBlock(texture.format) && texture.width >= 32 && texture.height >= 32 && texture.width <= 8192 &&
                   texture.height <= 8192;
        }
    }

    void PicturesOf(const art::alchemy::IgzFile& file, std::vector<Source>& out)
    {
        for (const auto& picture : file.Pictures())
        {
            art::Texture texture;
            if (!art::alchemy::TextureFormatOf(picture.format, texture))
                continue;
            texture.width = picture.width;
            texture.height = picture.height;
            texture.endian = art::TextureEndian::In16; // 8-in-16, the DXT formats' endian
            if (!Wanted(texture))
                continue;
            std::vector<uint8_t> pixels = file.Pixels(picture);
            // The storage as the console lays it out: tiled rows of 32-block tiles, or linear rows 32 blocks apart
            // (alchemy_archive.cpp, DecodePicture).
            uint32_t pitchBlocks = (texture.width + 3) / 4;
            pitchBlocks = (pitchBlocks + 31) / 32 * 32;
            Source source;
            if (FromConsole(pixels.data(), pixels.size(), texture, pitchBlocks, picture.name, source))
                out.push_back(std::move(source));
        }
    }

    void LuxPicturesOf(const std::vector<uint8_t>& stream, std::vector<Source>& out)
    {
        art::lux::ForEachPackage(stream, [&](const art::lux::Package& package) {
            const art::Texture& texture = package.texture;
            if (!Wanted(texture))
                return;
            // The fetch constant's pitch, as the renderer and xenos_texture.cpp's DecodeTexture read it: at least the
            // width; tiled, a multiple of 32 blocks; linear, of 256 bytes.
            uint32_t bytes = BlockBytes(texture.format);
            uint32_t pitchBlocks = std::max(std::max(texture.pitch, texture.width) / 4, (texture.width + 3) / 4);
            pitchBlocks =
                texture.tiled ? (pitchBlocks + 31) / 32 * 32 : (pitchBlocks * bytes + 255) / 256 * 256 / bytes;
            Source source;
            if (FromConsole(package.data, package.size, texture, pitchBlocks, package.name, source))
                out.push_back(std::move(source));
        });
    }

    bool ReadDumpFile(const std::filesystem::path& path, Source& out)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        dds::Info info;
        std::string error;
        if (!dds::Parse(bytes, bytes.size(), info, error))
            return false;
        switch (info.format)
        {
        case dds::Format::BC1: out.format = art::TextureFormat::Dxt1; break;
        case dds::Format::BC2: out.format = art::TextureFormat::Dxt3; break;
        case dds::Format::BC3: out.format = art::TextureFormat::Dxt5; break;
        case dds::Format::RGBA8: out.format = art::TextureFormat::Argb8; break;
        default: return false;
        }
        std::string stem = path.stem().string();
        if (stem.size() != 16 || stem.find_first_not_of("0123456789abcdef") != std::string::npos)
            return false;
        out.hash = std::stoull(stem, nullptr, 16);
        out.width = info.width;
        out.height = info.height;
        out.name = path.filename().string();
        uint64_t size = dds::SizeOfLevel(info.format, info.width, info.height, 0).Bytes();
        if (info.dataOffset + size > bytes.size())
            return false;
        out.data.assign(bytes.begin() + ptrdiff_t(info.dataOffset), bytes.begin() + ptrdiff_t(info.dataOffset + size));
        if (info.format == dds::Format::RGBA8)
            dds::ConvertTexels(info, out.data.data(), out.data.size());
        return true;
    }

    art::Image Decode(const Source& source)
    {
        art::Image image;
        image.width = source.width;
        image.height = source.height;
        image.rgba.resize(size_t(source.width) * source.height);
        if (!IsBlock(source.format))
        {
            memcpy(image.rgba.data(), source.data.data(), std::min(source.data.size(), image.rgba.size() * 4));
            return image;
        }
        uint32_t blocksX = (source.width + 3) / 4, blocksY = (source.height + 3) / 4, bytes = BlockBytes(source.format);
        for (uint32_t by = 0; by < blocksY; by++)
            for (uint32_t bx = 0; bx < blocksX; bx++)
            {
                uint32_t texels[16];
                art::DecodeBlock(&source.data[(size_t(by) * blocksX + bx) * bytes], source.format, texels);
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                        if (bx * 4 + x < source.width && by * 4 + y < source.height)
                            image.rgba[size_t(by * 4 + y) * source.width + bx * 4 + x] = texels[y * 4 + x];
            }
        return image;
    }

    Outcome Process(
        const Source& source, uint32_t maxSide, const std::filesystem::path& folder, const UpscaleOptions& options)
    {
        Outcome outcome;
        art::Image image = Decode(source);
        outcome.analysis = Analyse(image);
        outcome.sourceBytes = ChainBytes(DdsFormat(source.format), source.width, source.height);
        // An uncompressed texture (a dump's RGBA8, which is also how the dump writes the game's 8-bit and 16-bit formats)
        // is kept: BC1/BC3 would lose the precision its format was chosen for.
        if (outcome.analysis.kind != Kind::Color || !IsBlock(source.format))
            return outcome;
        if (std::max(source.width, source.height) > maxSide)
        {
            outcome.tooLarge = true;
            return outcome;
        }
        UpscaleOptions used = options;
        used.deblock = used.deblock && IsBlock(source.format);
        art::Image top = Upscale2x(image, outcome.analysis.alpha, used);
        std::vector<art::Image> levels = MipChain(top);
        levels.insert(levels.begin(), std::move(top));

        Alpha alpha = outcome.analysis.alpha;
        bool bc1 = alpha == Alpha::Opaque || (alpha == Alpha::Cutout && source.format == art::TextureFormat::Dxt1);
        dds::Format format = bc1 ? dds::Format::BC1 : dds::Format::BC3;
        std::vector<std::vector<uint8_t>> encoded;
        for (const art::Image& level : levels)
            encoded.push_back(bc1 ? EncodeBc1(level, alpha != Alpha::Opaque) : EncodeBc3(level));
        std::vector<std::span<const uint8_t>> spans(encoded.begin(), encoded.end());
        std::vector<uint8_t> file = dds::Write(format, levels[0].width, levels[0].height, spans);

        std::filesystem::path path = folder / (texpack::HashName(source.hash) + ".dds");
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
            if (!out)
                return outcome;
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
            return outcome;
        outcome.written = true;
        outcome.packBytes = ChainBytes(format, levels[0].width, levels[0].height);
        return outcome;
    }
}
