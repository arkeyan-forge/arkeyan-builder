// HD textures, the pack side (docs/tools/hd_textures.md): Arkeyan-Builder's optional step that doubles the game's own
// textures, read from the player's disc (Alchemy archives) and from the game's texture dump, into the player's pack
// folder <game data>/textures-hd/ (docs/runtime/gpu/texture_packs.md). Nothing of it is shipped or kept in git.
#pragma once

#include "hd_upscale.h"
#include "host/art/alchemy_archive.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hdtex
{
    // off: no pack (an existing one is removed). 2x: every texture that gains from it, up to 1024 texels a side.
    // auto: the same up to a side the machine's memory affords (MaxSide). A level holds about 40-75 MB more video
    // memory at 2x (docs/tools/hd_textures.md), so only a machine under 8 GB is held to 512.
    enum class Mode
    {
        Off,
        Double,
        Auto,
    };
    bool ParseMode(std::string_view text, Mode& mode);
    const char* ModeName(Mode mode);

    // The longest side a texture may have to be doubled: 2x 1024; auto 1024 with 8 GB of memory or more, 512 below.
    uint32_t MaxSide(Mode mode, uint64_t physicalMemory);

    // A texture's top level as the renderer uploads it: rows of 4x4 blocks (DXT1, DXT3, DXT5) or of RGBA8 texels (R
    // in the low byte), untiled and byte-swapped. `hash` is its pack name (texpack::ContentHash).
    struct Source
    {
        uint64_t hash = 0;
        art::TextureFormat format = art::TextureFormat::Dxt5;
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> data;
        std::string name; // the source path, for the manifest; may be empty
    };

    // The Xenos texture format number the renderer hashes with (18 DXT1, 19 DXT2_3, 20 DXT4_5, 6 8_8_8_8).
    uint32_t XenosFormat(art::TextureFormat format);

    // The DXT pictures of an IGZ file whose layout is known, as Sources (smaller than 32 texels a side: skipped, their
    // top level may sit in the packed mip tail).
    void PicturesOf(const art::alchemy::IgzFile& file, std::vector<Source>& out);

    // [Lux] The DXT textures of an unpacked "strm" stream (levels/<name>/mission.str, the UI streams) or of a bare APKF
    // file (textures/*.apk), as Sources, with the same rule.
    void LuxPicturesOf(const std::vector<uint8_t>& stream, std::vector<Source>& out);

    // A file of the game's own dump (<game data>/textures-dump/<hash>.dds: BC1-BC3 or RGBA8); false for another format.
    bool ReadDumpFile(const std::filesystem::path& path, Source& out);

    art::Image Decode(const Source& source);

    struct Outcome
    {
        Analysis analysis;
        bool written = false;
        bool tooLarge = false;       // a texture that gains, over the mode's side
        uint64_t sourceBytes = 0;    // the game's texture with a full mip chain
        uint64_t packBytes = 0;      // the replacement, its mips included (what the GPU holds instead)
    };

    // Analyses the texture and, when it gains (a DXT one) and its sides are at most `maxSide`, writes <folder>/<hash>.dds: the
    // doubled picture with a full mip chain, BC1 when opaque (or a DXT1 cut-out), else BC3.
    Outcome Process(const Source& source, uint32_t maxSide, const std::filesystem::path& folder,
        const UpscaleOptions& options = {});
}
