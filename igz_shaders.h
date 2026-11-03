// Alchemy shaders straight from the disc, without Python or a run of the game: Giants (IGA v8, IGZ v6;
// games/giants/docs/shaders.md), Swap Force (IGA v10, IGZ v7; games/swapforce/docs/shaders.md) and Trap Team (IGA v11,
// IGZ v8).
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>
#include <set>
#include <string>

struct ShaderExtraction
{
    size_t entries = 0;   // files in the archive
    size_t igz = 0;       // of which IGZ v6, v7 or v8 files decoded
    size_t vertex = 0;    // distinct shader containers written
    size_t pixel = 0;
    size_t unsupported = 0; // entries this reader cannot inspect (unreadable data or unknown IGZ versions)
};

// Opens an IGA v8, v10 or v11 archive (the bytes of a .arc / .bld / .pak), decodes each entry that is an IGZ v6, v7 or v8 file,
// and writes every Xenos shader container it holds to <outDir>/<hash>.vs|.ps, exactly as the game hands it to D3DVertexShader_Init /
// D3DPixelShader_Init (the virtual part followed by the physical part). Equal containers are written once.
// False when `archive` is unreadable or an output write fails. Unsupported IGZ entries are counted for a warning.
// "igz:<path>" selects exactly one archive; "igz:all" selects every .arc/.bld file (paths from DiscFileSystem::Files).
std::vector<std::string> ShaderArchivePaths(const std::string& recipe, const std::vector<std::string>& files);

// Pass a shared set for a multi-archive scan: duplicates across archives are written and counted once.
bool ExtractArchiveShaders(const std::vector<uint8_t>& archive, const std::filesystem::path& outDir,
    ShaderExtraction& result, std::set<std::string>* sharedSeen = nullptr);

// ExtractArchiveShaders in two halves, so that several archives can be decoded at once and written in order: Collect
// decodes (entries and igz of `result`) and keeps every container found as { file name, bytes }; Write writes and counts
// (vertex and pixel) those not in `seen` yet.
struct ArchiveShader
{
    std::string name, bytes;
};
bool CollectArchiveShaders(
    const std::vector<uint8_t>& archive, ShaderExtraction& result, std::vector<ArchiveShader>& shaders);
bool WriteArchiveShaders(const std::vector<ArchiveShader>& shaders, const std::filesystem::path& outDir,
    ShaderExtraction& result, std::set<std::string>& seen);
