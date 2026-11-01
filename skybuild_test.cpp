// Disc-free checks of game selection, shader recipes and synthetic IGA/IGZ containers.
#include "game_config.h"
#include "igz_shaders.h"
#include "sha256.h"
#include <cstdio>
#include <cstring>
#include <iterator>

namespace fs = std::filesystem;
static int checks = 0, failures = 0;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            ++failures;                                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
        }                                                                                                              \
    } while (0)

static void Put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value, bool big = true)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = uint8_t(value >> (8 * (big ? 3 - i : i)));
}

// Two invented containers, not executable shaders. The physical bytes distinguish their order.
static std::vector<uint8_t> MakeIgz(unsigned version, std::vector<std::string>& containers)
{
    std::vector<uint8_t> bytes(0x400);
    memcpy(bytes.data(), "IGZ\x01", 4);
    Put32(bytes, 4, version);
    const size_t table = version == 6 ? 0x10 : 0x18;
    Put32(bytes, table, 0x200);
    Put32(bytes, table + 4, 0x40);
    Put32(bytes, table + 16, 0x280);
    Put32(bytes, table + 20, 0x100);
    if (version != 6)
    {
        Put32(bytes, table + 32, 0x380);
        Put32(bytes, table + 36, 0x10);
        memcpy(&bytes[0x200], "NHMT", 4);
        Put32(bytes, 0x204, 2);
        Put32(bytes, 0x208, 0x40);
        Put32(bytes, 0x20C, 0x10);
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        size_t p = 0x280 + i * (version == 6 ? 0x48 : 0x40);
        size_t physical = version == 6 ? p + 0x40 : 0x380 + i * 8;
        size_t handle = 0x210 + i * 8;
        Put32(bytes, handle, 0x18000008);
        Put32(bytes, handle + 4, version == 6 ? uint32_t(physical - 0x280) : (1u << 27) + i * 8);
        Put32(bytes, p, 0x102A1100 + i); // pixel, then vertex
        Put32(bytes, p + 4, 0x40);
        Put32(bytes, p + 8, 8);
        std::fill(bytes.begin() + physical, bytes.begin() + physical + 8, uint8_t(0x60 + i));
        std::string container(reinterpret_cast<const char*>(&bytes[p]), 0x40);
        container.append(reinterpret_cast<const char*>(&bytes[physical]), 8);
        containers.push_back(container);
    }
    return bytes;
}

static std::vector<uint8_t> MakeIga(unsigned version, const std::vector<uint8_t>& igz)
{
    std::vector<uint8_t> bytes(0x100);
    bool big = version == 10;
    memcpy(bytes.data(),
        big ? "\x1a"
              "AGI"
            : "IGA\x1a",
        4);
    Put32(bytes, 4, version, big);
    Put32(bytes, 0x0C, 1, big);
    size_t entry = (version == 8 ? 0x34 : 0x38) + 4;
    if (version == 10)
        entry += 4;
    Put32(bytes, entry, 0x100, big);
    Put32(bytes, entry + (version == 11 ? 8 : 4), uint32_t(igz.size()), big);
    Put32(bytes, entry + (version == 11 ? 12 : 8), 0xFFFFFFFF, big);
    bytes.insert(bytes.end(), igz.begin(), igz.end());
    return bytes;
}

static void TestShaders(const fs::path& scratch)
{
    std::vector<std::string> files = { "level/a.arc", "level/a.bld", "permanent/global.arc", "permanent/global.bld",
        "default.xex", "movies/a.mov", "archives/shaders_xenon.pak" };
    CHECK(ShaderArchivePaths("igz:all", files) == std::vector<std::string>(files.begin(), files.begin() + 4));
    CHECK(ShaderArchivePaths("igz:all", {}).empty());
    CHECK(ShaderArchivePaths("lsa", files).empty());
    for (const std::string path : { "/permanent/global.bld", "/archives/shaders_xenon.pak" })
        CHECK(ShaderArchivePaths("igz:" + path, files) == std::vector<std::string>{ path });

    for (unsigned version : { 6u, 7u, 8u })
    {
        fs::path out = scratch / std::to_string(version);
        fs::remove_all(out);
        std::vector<std::string> containers;
        auto igz = MakeIgz(version, containers);
        auto archive = MakeIga(version == 6 ? 8 : version == 7 ? 10 : 11, igz);
        ShaderExtraction result;
        std::set<std::string> seen;
        CHECK(ExtractArchiveShaders(archive, out, result, &seen));
        CHECK(result.entries == 1 && result.igz == 1 && result.vertex == 1 && result.pixel == 1);
        for (size_t i = 0; i < containers.size(); ++i)
        {
            std::ifstream file(
                out / (Sha256Text(containers[i]).substr(0, 16) + (i == 0 ? ".ps" : ".vs")), std::ios::binary);
            std::string actual{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            CHECK(actual == containers[i]);
        }
        // A second archive with identical containers contributes no extra shaders.
        ShaderExtraction repeated;
        CHECK(ExtractArchiveShaders(archive, out, repeated, &seen));
        CHECK(repeated.igz == 1 && repeated.vertex == 0 && repeated.pixel == 0 && seen.size() == 2);
        // The next archive repeats one container and adds another: retain only the changed one.
        size_t pixelPhysical = version == 6 ? 0x2C0 : 0x380;
        igz[pixelPhysical] ^= 1;
        ShaderExtraction changed;
        CHECK(ExtractArchiveShaders(MakeIga(11, igz), out, changed, &seen));
        CHECK(changed.pixel == 1 && changed.vertex == 0 && seen.size() == 3);
        // Archives with no shader are normal during a full disc walk.
        Put32(igz, 0x280, 0);
        Put32(igz, version == 6 ? 0x2C8 : 0x2C0, 0);
        ShaderExtraction empty;
        CHECK(ExtractArchiveShaders(MakeIga(11, igz), out, empty, &seen));
        CHECK(empty.igz == 1 && empty.vertex == 0 && empty.pixel == 0);
        // A valid archive with an unsupported IGZ contributes nothing; malformed IGA is refused.
        Put32(igz, 4, 99);
        ShaderExtraction unsupported;
        CHECK(ExtractArchiveShaders(MakeIga(11, igz), out, unsupported));
        CHECK(unsupported.igz == 0 && unsupported.vertex == 0 && unsupported.pixel == 0);
        CHECK(!ExtractArchiveShaders({}, out, unsupported));
        fs::remove_all(out);
    }
}

static void TestGames(const fs::path& root)
{
    using namespace skybuild;
    std::vector<Game> games;
    for (const std::string id : { "ssa", "giants", "swapforce", "trapteam" })
    {
        auto values = ReadKeyValues(root / "games" / id / "game.txt");
        CHECK(values["id"] == id);
        games.push_back(ReadGame(id, values));
        CHECK(!games.back().xexSha256.empty());
        CHECK(fs::exists(root / "games" / id / "config" / games.back().toml));
        std::ifstream file(root / "games" / id / "info.cpp");
        std::string info{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        for (const auto& hash : games.back().xexSha256)
            CHECK(hash.size() == 64 && info.find(hash) != std::string::npos);
    }
    for (const auto& game : games)
        for (const auto& hash : game.xexSha256)
            CHECK(FindGame(games, hash) == &game);
    CHECK(FindGame(games, std::string(64, '0')) == nullptr);
    CHECK(FindGame({}, games.back().xexSha256.front()) == nullptr);
    CHECK(games[0].shaders == "lsa" && games[0].runtime == "SSARecomp");
    CHECK(games[1].shaders == "igz:/permanent/global.bld" && games[1].runtime == "SGRecomp");
    CHECK(games[2].shaders == "igz:/archives/shaders_xenon.pak" && games[2].runtime == "SSFRecomp");
    CHECK(games[2].xexSha256.size() == 2);
    CHECK(games[0].moviePortal.empty() && games[0].moviePress.empty() && games[0].movieShaders == 3);
    CHECK(games[1].moviePortal.empty() && games[1].moviePress.empty() && games[1].movieShaders == 50);
    CHECK(games[2].moviePortal == "emulated" && !games[2].moviePress.empty() && games[2].movieShaders == 7);
    CHECK(games[3].shaders == "igz:all" && games[3].runtime == "TTRecomp");
    CHECK(games[3].moviePortal == "emulated" && games[3].movieShaders == 18);
    // A short pass of an older kit is not done, so an update repairs the game.
    CHECK(!MoviePassDone("45\n", 50));
    CHECK(MoviePassDone("50\n", 50));
    CHECK(MoviePassDone("51\n", 50));
    CHECK(!MoviePassDone("", 50));
    CHECK(MoviePassDone("3\n", 0));
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    TestGames(argv[1]);
    TestShaders(argv[2]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
