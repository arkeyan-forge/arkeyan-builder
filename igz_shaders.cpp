// See igz_shaders.h. The formats are documented in games/giants/docs/shaders.md and games/swapforce/docs/shaders.md
// (reverse-engineered from the discs); the archive reader (IGA v8, v10 and v11, the chunked LZMA) is the runtime's,
// host/art/alchemy_archive.h.
#include "igz_shaders.h"
#include "host/art/alchemy_archive.h"
#include "sha256.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>

namespace
{
    uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

    // A D3D shader container's header at p: 10 2A 11 00 (pixel) / 01 (vertex), virtual size, physical size.
    bool IsContainer(const std::vector<uint8_t>& igz, size_t p, uint32_t& virtualSize, uint32_t& physical)
    {
        if (igz[p] != 0x10 || igz[p + 1] != 0x2A || igz[p + 2] != 0x11 || igz[p + 3] > 1)
            return false;
        virtualSize = Be32(&igz[p + 4]), physical = Be32(&igz[p + 8]);
        return virtualSize >= 0x40 && virtualSize <= 0x4000 && virtualSize % 4 == 0;
    }

    // --- IGZ v6 (Giants) ---------------------------------------------------------------------------------------------

    template <typename Callback>
    void ForEachContainerV6(const std::vector<uint8_t>& igz, Callback&& callback)
    {
        struct Section
        {
            uint32_t offset, size;
        };
        std::vector<Section> sections;
        for (size_t i = 0; i < 16 && 0x10 + 16 * (i + 1) <= igz.size(); i++)
        {
            Section s{ Be32(&igz[0x10 + 16 * i]), Be32(&igz[0x14 + 16 * i]) };
            if (s.size)
                sections.push_back(s);
        }
        if (sections.empty())
            return;

        // The physical (microcode) blocks, by size: pairs { 0x18<<24 | size, section<<24 | offset } in section 0.
        std::map<uint32_t, std::set<uint64_t>> blocks;
        size_t start = sections[0].offset, stop = std::min<size_t>(start + sections[0].size, igz.size());
        for (size_t i = start; i + 8 < stop; i += 4)
        {
            uint32_t word = Be32(&igz[i]), rel = Be32(&igz[i + 4]);
            size_t index = (rel >> 24) + 1;
            uint32_t size = word & 0xFFFFFF;
            if (word >> 24 == 0x18 && size > 0 && size < 0x100000 && index < sections.size())
                blocks[size].insert(uint64_t(sections[index].offset) + (rel & 0xFFFFFF));
        }

        // The containers: D3D headers 10 2A 11 00 (pixel) / 01 (vertex), virtual size, physical size.
        for (size_t p = 0; p + 12 <= igz.size(); p++)
        {
            uint32_t virtualSize, physical;
            if (!IsContainer(igz, p, virtualSize, physical))
                continue;
            auto found = blocks.find(physical);
            if (found == blocks.end())
                continue;
            uint64_t near = 0;
            int count = 0;
            for (uint64_t a : found->second)
                if (a >= p + virtualSize && a < p + virtualSize + 0x40)
                    near = a, count++;
            if (count == 1 && near + physical <= igz.size() && p + virtualSize <= igz.size())
            {
                std::string blob(reinterpret_cast<const char*>(&igz[p]), virtualSize);
                blob.append(reinterpret_cast<const char*>(&igz[near]), physical);
                callback(blob, (igz[p + 3] & 1) != 0);
            }
        }
    }

    // --- IGZ v7 (Swap Force) -----------------------------------------------------------------------------------------

    // Sections at 0x14, { u32 ?, offset, size, alignment }; section 0 holds tagged records { tag, count, length, data
    // start }, the tags stored byte-swapped. TMHN lists the memory handles { kind<<24 | size, pool<<27 | offset } (pool
    // n is section n + 1); kind 0x18 is physical memory. A shader's virtual part sits in an object pool and its physical
    // part in the 0x20-aligned pool, far apart, but the containers and their physical blocks come in the same order: each
    // container takes the next physical block of its physical size [V: all 1074 of the disc, 549 checked against dumps].
    template <typename Callback>
    void ForEachContainerV7(const std::vector<uint8_t>& igz, Callback&& callback)
    {
        struct Section
        {
            uint32_t offset, size;
        };
        std::vector<Section> sections;
        for (size_t i = 0; i < 16 && 0x14 + 16 * (i + 1) <= igz.size(); i++)
        {
            Section s{ Be32(&igz[0x18 + 16 * i]), Be32(&igz[0x1C + 16 * i]) };
            if (s.size == 0)
                break;
            sections.push_back(s);
        }
        if (sections.empty())
            return;

        struct Block
        {
            uint32_t size;
            uint64_t offset;
        };
        std::vector<Block> physicalBlocks;
        size_t o = sections[0].offset,
               stop = std::min<size_t>(size_t(sections[0].offset) + sections[0].size, igz.size());
        while (o + 16 <= stop)
        {
            uint32_t count = Be32(&igz[o + 4]), length = Be32(&igz[o + 8]), start = Be32(&igz[o + 12]);
            if (length < 16)
                break;
            if (memcmp(&igz[o], "NHMT", 4) == 0)
                for (uint32_t i = 0; i < count && o + start + 8 * (i + 1) <= stop; i++)
                {
                    uint32_t word = Be32(&igz[o + start + 8 * i]), rel = Be32(&igz[o + start + 8 * i + 4]);
                    size_t index = (rel >> 27) + 1;
                    if (word >> 24 == 0x18 && index < sections.size())
                        physicalBlocks.push_back(
                            { word & 0xFFFFFF, uint64_t(sections[index].offset) + (rel & 0x7FFFFFF) });
                }
            o += length;
        }

        size_t next = 0;
        for (size_t p = 0; p + 12 <= igz.size(); p++)
        {
            uint32_t virtualSize, physical;
            if (!IsContainer(igz, p, virtualSize, physical))
                continue;
            size_t j = next;
            while (j < physicalBlocks.size() && physicalBlocks[j].size != physical)
                j++;
            if (j == physicalBlocks.size())
                continue;
            next = j + 1;
            uint64_t at = physicalBlocks[j].offset;
            if (at + physical <= igz.size() && p + virtualSize <= igz.size())
            {
                std::string blob(reinterpret_cast<const char*>(&igz[p]), virtualSize);
                blob.append(reinterpret_cast<const char*>(&igz[at]), physical);
                callback(blob, (igz[p + 3] & 1) != 0);
            }
        }
    }
}

std::vector<std::string> ShaderArchivePaths(const std::string& recipe, const std::vector<std::string>& files)
{
    if (recipe.rfind("igz:", 0) != 0)
        return {};
    if (recipe != "igz:all")
        return { recipe.substr(4) };
    std::vector<std::string> paths;
    for (const auto& path : files)
    {
        auto extension = std::filesystem::path(path).extension();
        if (extension == ".arc" || extension == ".bld")
            paths.push_back(path);
    }
    return paths;
}

bool CollectArchiveShaders(
    const std::vector<uint8_t>& archive, ShaderExtraction& result, std::vector<ArchiveShader>& shaders)
{
    auto iga = art::alchemy::IgaArchive::Open(
        [&archive](uint64_t offset, size_t size, uint8_t* out) {
            if (offset + size > archive.size())
                return false;
            memcpy(out, archive.data() + offset, size);
            return true;
        },
        archive.size());
    if (!iga)
        return false;
    result.entries = iga->Count();
    for (size_t i = 0; i < iga->Count(); i++)
    {
        if (iga->Size(i) < 0x40)
            continue;
        auto prefix = iga->Read(i, 4);
        if (prefix.empty())
        {
            result.unsupported++; // cannot determine whether this entry contains shaders
            continue;
        }
        if (prefix != std::vector<uint8_t>{ 'I', 'G', 'Z', 1 })
            continue;
        std::vector<uint8_t> data = iga->Read(i);
        if (data.size() < 0x40 || memcmp(data.data(), "IGZ\x01", 4) != 0)
            return false;
        uint32_t version = Be32(&data[4]);
        if (version != 6 && version != 7 && version != 8)
        {
            result.unsupported++;
            continue;
        }
        result.igz++;
        auto keep = [&](const std::string& blob, bool vertex) {
            shaders.push_back({ Sha256Text(blob).substr(0, 16) + (vertex ? ".vs" : ".ps"), blob });
        };
        if (version == 6)
            ForEachContainerV6(data, keep);
        else
            ForEachContainerV7(data, keep); // v8 (Trap Team) has v7's section table and memory handles [V]
    }
    return true;
}

bool WriteArchiveShaders(const std::vector<ArchiveShader>& shaders, const std::filesystem::path& outDir,
    ShaderExtraction& result, std::set<std::string>& seen)
{
    std::filesystem::create_directories(outDir);
    for (const ArchiveShader& shader : shaders)
    {
        if (seen.count(shader.name))
            continue;
        std::ofstream file(outDir / shader.name, std::ios::binary | std::ios::trunc);
        file.write(shader.bytes.data(), std::streamsize(shader.bytes.size()));
        file.close();
        if (!file)
            return false;
        seen.insert(shader.name);
        (shader.name.compare(shader.name.size() - 3, 3, ".vs") == 0 ? result.vertex : result.pixel)++;
    }
    return true;
}

bool ExtractArchiveShaders(const std::vector<uint8_t>& archive, const std::filesystem::path& outDir,
    ShaderExtraction& result, std::set<std::string>* sharedSeen)
{
    std::vector<ArchiveShader> shaders;
    if (!CollectArchiveShaders(archive, result, shaders))
        return false;
    std::set<std::string> localSeen;
    return WriteArchiveShaders(shaders, outDir, result, sharedSeen ? *sharedSeen : localSeen);
}
