// Alchemy shaders straight from the disc, without Python or a run of the game: Giants (IGA v8, IGZ v6;
// games/giants/docs/shaders.md) and Swap Force (IGA v10, IGZ v7; games/swapforce/docs/shaders.md).
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

struct ShaderExtraction
{
    size_t entries = 0;   // files in the archive
    size_t igz = 0;       // of which IGZ v6 or v7 files decoded
    size_t vertex = 0;    // distinct shader containers written
    size_t pixel = 0;
};

// Opens an IGA v8 or v10 archive (the bytes of a .arc / .bld / .pak), decodes each entry that is an IGZ v6 or v7 file,
// and writes every Xenos shader container it holds to <outDir>/<hash>.vs|.ps, exactly as the game hands it to D3DVertexShader_Init /
// D3DPixelShader_Init (the virtual part followed by the physical part). Equal containers are written once.
// False when `archive` is not an IGA archive the runtime's reader knows.
bool ExtractArchiveShaders(
    const std::vector<uint8_t>& archive, const std::filesystem::path& outDir, ShaderExtraction& result);
