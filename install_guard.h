// Install freshness and completeness checks; inputs are read only.
#pragma once
#include "sha256.h"
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace skybuild
{
    inline std::set<uint64_t> ShaderCacheHashes(const std::filesystem::path& cache)
    {
        std::set<uint64_t> hashes;
        std::ifstream file(cache);
        for (std::string line; std::getline(file, line);)
        {
            constexpr std::string_view prefix = "\t{ 0x";
            if (!line.starts_with(prefix))
                continue;
            uint64_t hash = 0;
            const char* end = line.data() + line.size();
            auto parsed = std::from_chars(line.data() + prefix.size(), end, hash, 16);
            if (parsed.ec == std::errc{} && parsed.ptr != end && *parsed.ptr == ',')
                hashes.insert(hash);
        }
        return hashes;
    }

    // Ignore partial dumps, unrelated files and containers whose header disagrees with their extension or size.
    inline std::map<std::string, std::filesystem::path> RuntimeShaders(const std::filesystem::path& directory)
    {
        std::map<std::string, std::filesystem::path> files;
        std::error_code error;
        if (!std::filesystem::exists(directory, error))
            return files;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
        {
            const auto& path = entry.path();
            const std::string name = path.filename().string(), stem = path.stem().string();
            const std::string extension = path.extension().string();
            if (!entry.is_regular_file() || stem.size() != 16 || (extension != ".vs" && extension != ".ps"))
                continue;
            uint64_t hash = 0;
            auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), hash, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != stem.data() + stem.size())
                continue;
            unsigned char header[12]{};
            std::ifstream file(path, std::ios::binary);
            if (!file.read(reinterpret_cast<char*>(header), sizeof(header)))
                continue;
            auto word = [&](size_t at) {
                return (uint32_t(header[at]) << 24) | (uint32_t(header[at + 1]) << 16) |
                       (uint32_t(header[at + 2]) << 8) | uint32_t(header[at + 3]);
            };
            const uint32_t flags = word(0), virtualSize = word(4), physicalSize = word(8);
            if (flags == (extension == ".vs" ? 0x102A1101u : 0x102A1100u) && virtualSize >= 0x40 &&
                virtualSize % 4 == 0 && physicalSize > 0 && entry.file_size() == uint64_t(virtualSize) + physicalSize)
                files.emplace(name, path);
        }
        return files;
    }

    // A deterministic inventory of disc inputs, excluding the separately captured run-time shaders.
    inline std::string DiscShaderInventory(const std::filesystem::path& directory)
    {
        std::map<std::string, std::string> files;
        std::error_code error;
        if (!std::filesystem::exists(directory, error))
            return {};
        for (std::filesystem::recursive_directory_iterator it(directory), end; it != end; ++it)
        {
            if (it->path() == directory / "runtime")
            {
                if (it->is_directory())
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file())
                files[it->path().lexically_relative(directory).generic_string()] = Sha256File(it->path());
        }
        std::string inventory;
        for (const auto& [name, hash] : files)
            inventory += name + "=" + hash + "\n";
        return inventory;
    }
}
