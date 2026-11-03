// Disc-free checks of game selection, shader recipes and synthetic IGA/IGZ containers.
#include "game_config.h"
#include "game_icon.h"
#include <stb_image.h>
#include "igz_shaders.h"
#include "sha256.h"
#include "shader_cache_literals.h"
#include <algorithm>
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
        CHECK(unsupported.unsupported == 1);
        CHECK(!ExtractArchiveShaders({}, out, unsupported));
        fs::remove_all(out);
        // A failed output write must not count as a successful extraction (even when another shader writes).
        fs::create_directories(out / (Sha256Text(containers[0]).substr(0, 16) + ".ps"));
        ShaderExtraction unwritable;
        CHECK(!ExtractArchiveShaders(archive, out, unwritable));
        CHECK(unwritable.pixel == 0);
        fs::remove_all(out);
    }
}

// The concatenated string literals of `text` from `at` to the next ';', escapes decoded as a compiler reads them.
static std::string DecodeLiterals(const std::string& text, size_t at)
{
    std::string bytes;
    bool inside = false;
    for (size_t i = at; i < text.size() && (inside || text[i] != ';'); i++)
    {
        if (text[i] == '"')
            inside = !inside;
        else if (inside && text[i] == '\\')
        {
            unsigned value = 0;
            for (int digit = 0; digit < 3 && text[i + 1] >= '0' && text[i + 1] <= '7'; digit++)
                value = value * 8 + unsigned(text[++i] - '0');
            bytes += char(value);
        }
        else if (inside)
            bytes += text[i];
    }
    return bytes;
}

static void TestShaderCacheLiterals()
{
    // Every byte value, then an escape followed by digits (0x01 '7'), then quote, backslash and '?' twice.
    std::string expected;
    for (unsigned i = 0; i < 9000; i++)
        expected += char(i % 256);
    expected += "\x01"
                "7\"\\??";
    std::string list;
    for (unsigned char byte : expected)
        list += std::to_string(byte) + ",";
    std::string text =
        "#include \"shader_cache.h\"\nShaderCacheEntry g_shaderCacheEntries[] = {\n\t{ 0x1, 0 },\n};\n"
        "const uint8_t g_compressedAirCache[] = {" +
        list + "};\nconst size_t g_airCacheCompressedSize = 9006;\nconst uint8_t g_compressedSpirvCache[] = {};\n";
    std::string out = ShaderCacheLiterals(text);
    size_t air = out.find("g_compressedAirCache[] = ");
    size_t spirv = out.find("g_compressedSpirvCache[] = ");
    CHECK(air != std::string::npos && spirv != std::string::npos);
    CHECK(DecodeLiterals(out, air) == expected);
    CHECK(DecodeLiterals(out, spirv).empty());
    CHECK(out.find("g_shaderCacheEntries[] = {\n\t{ 0x1, 0 },\n};") != std::string::npos);
    CHECK(out.find("const size_t g_airCacheCompressedSize = 9006;") != std::string::npos);
    CHECK(out.find("= {") == out.rfind("= {")); // the entries' list only
    // Lines stay short; a file that is not a list of numbers is kept as it is.
    size_t longest = 0;
    for (size_t at = 0, end; at < out.size(); at = end + 1)
        longest = std::max(longest, (end = std::min(out.find('\n', at), out.size())) - at);
    CHECK(longest < 4 * 4096 + 8);
    CHECK(ShaderCacheLiterals("const uint8_t g_x Cache[] = {1, 2};") == "const uint8_t g_x Cache[] = {1, 2};");
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
        CHECK(info.find("\"" + games.back().runtime + "\" };") != std::string::npos);
        for (const auto& hash : games.back().xexSha256)
            CHECK(hash.size() == 64 && info.find(hash) != std::string::npos);
    }
    for (const auto& game : games)
        for (const auto& hash : game.xexSha256)
            CHECK(FindGame(games, hash) == &game);
    for (const auto& game : games)
        CHECK(game.name == game.runtime);
    CHECK(DesktopArgument("/jeux/été's folder/Skylanders Spyro's Adventure") ==
          "\"/jeux/été's folder/Skylanders Spyro's Adventure\"");
    CHECK(DesktopArgument("/games/100%") == "\"/games/100%%\"");
    CHECK(GameDesktopEntry(games[0].name, "/games/Spyro's Adventure", "/games/game_icon.png")
              .find("\nName=Skylanders Spyro's Adventure\nExec=\"/games/Spyro's Adventure\"\n") != std::string::npos);
    CHECK(FindGame(games, std::string(64, '0')) == nullptr);
    CHECK(FindGame({}, games.back().xexSha256.front()) == nullptr);
    CHECK(games[0].shaders == "lsa" && games[0].runtime == "Skylanders Spyro's Adventure");
    CHECK(games[1].shaders == "igz:/permanent/global.bld" && games[1].runtime == "Skylanders Giants");
    CHECK(games[2].shaders == "igz:/archives/shaders_xenon.pak" && games[2].runtime == "Skylanders Swap Force");
    CHECK(games[2].xexSha256.size() == 2);
    CHECK(games[0].moviePortal.empty() && games[0].moviePress.empty() && games[0].movieShaders == 3);
    CHECK(games[1].moviePortal.empty() && games[1].moviePress.empty() && games[1].movieShaders == 50);
    CHECK(games[2].moviePortal == "emulated" && !games[2].moviePress.empty() && games[2].movieShaders == 7);
    CHECK(games[3].shaders == "igz:all" && games[3].runtime == "Skylanders Trap Team");
    CHECK(games[3].moviePortal == "emulated" && games[3].movieShaders == 18);
    // A short pass of an older kit is not done, so an update repairs the game.
    CHECK(!MoviePassDone("45\n", 50));
    CHECK(MoviePassDone("50\n", 50));
    CHECK(MoviePassDone("51\n", 50));
    CHECK(!MoviePassDone("", 50));
    CHECK(MoviePassDone("3\n", 0));
    // A program of the kit the antivirus stopped: the install says which, and what to do.
    CHECK(StartErrorHint(0, "XenosRecomp").empty());
    CHECK(StartErrorHint(225, "XenosRecomp").find("Windows Security blocked XenosRecomp") != std::string::npos);
    CHECK(StartErrorHint(2, "XenosRecomp").find("XenosRecomp is missing") != std::string::npos);
    CHECK(StartErrorHint(1234, "XenosRecomp") == " (Windows error 1234)");
    CHECK(!MoviePassDone("", 0));
    CHECK(!MoviePassDone("0\n", 0));
    CHECK(!MoviePassDone("-1\n", 50));
    CHECK(!MoviePassDone("50garbage", 50));
}

static void TestUpgrade(const fs::path& root, const fs::path& scratch)
{
    using namespace skybuild;
    for (const std::string id : { "ssa", "giants", "swapforce", "trapteam" })
    {
        const auto game = ReadGame(id, ReadKeyValues(root / "games" / id / "game.txt"));
        for (const std::string suffix : { "", ".exe", ".app" })
        {
            auto folder = scratch / fs::path(u8"été's games") / id;
            auto old = folder / (LegacyRuntime(id) + suffix);
            auto current = folder / (game.runtime + suffix);
            auto executable = [&](const fs::path& path) {
                return suffix == ".app" ? path / "Contents" / "MacOS" / path.stem() : path;
            };
            auto write = [](const fs::path& path, const char* text) {
                fs::create_directories(path.parent_path());
                std::ofstream(path) << text;
            };
            write(executable(old), "old executable");
            write(folder / "shortcut.ico", "shortcut icon");
            write(folder / "settings.ini", "settings");
            write(folder / "another game", "unrelated executable");
            RemoveLegacyRuntime(game, folder, suffix); // failed or interrupted link: keep old
            CHECK(fs::exists(executable(old)));
            write(executable(current), "new executable");
            if (suffix == ".app")
                write(folder / LegacyRuntime(id), "old command-line entry");
            RemoveLegacyRuntime(game, folder, suffix);
            CHECK(!fs::exists(old) && fs::exists(executable(current)));
            CHECK(suffix != ".app" || !fs::exists(folder / LegacyRuntime(id)));
            CHECK(fs::exists(folder / "shortcut.ico") && fs::exists(folder / "settings.ini"));
            CHECK(fs::exists(folder / "another game"));
            RemoveLegacyRuntime(game, folder, suffix); // repeated up-to-date builds are safe
            CHECK(fs::exists(executable(current)));
            fs::remove_all(folder);
        }
    }
}

// An install made by an older kit keeps its ssabuild-work in place (no rebuild for the rename); a new one uses arkeyan-work.
static void TestWorkFolder(const fs::path& scratch)
{
    using namespace skybuild;
    const fs::path fresh = scratch / "work-fresh", old = scratch / "work-old";
    CHECK(WorkFolder(fresh) == fresh / "arkeyan-work");
    fs::create_directories(old / "ssabuild-work" / "ppc");
    CHECK(WorkFolder(old) == old / "ssabuild-work");
    fs::create_directories(old / "arkeyan-work");
    CHECK(WorkFolder(old) == old / "ssabuild-work");
    CHECK(fs::exists(old / "ssabuild-work" / "ppc"));
    fs::remove_all(fresh);
    fs::remove_all(old);
}

static void TestIcons(const fs::path& scratch)
{
    art::Image source;
    source.width = 80;
    source.height = 40;
    source.rgba.resize(80 * 40);
    // Synthetic off-centre rectangle with transparent padding; never disc artwork.
    for (unsigned y = 9; y < 29; ++y)
        for (unsigned x = 20; x < 60; ++x)
            source.rgba[y * 80 + x] = 0xFF204080;
    auto decode = [&](const uint8_t* bytes, size_t length, unsigned size) {
        int width = 0, height = 0, channels = 0;
        auto* pixels = stbi_load_from_memory(bytes, int(length), &width, &height, &channels, 4);
        CHECK(pixels != nullptr && width == int(size) && height == int(size));
        if (pixels)
        {
            CHECK(pixels[3] == 0); // transparent corner
            size_t centre = (size_t(size / 2) * size + size / 2) * 4;
            CHECK(pixels[centre] == 0x80 && pixels[centre + 1] == 0x40 && pixels[centre + 2] == 0x20);
            unsigned left = size, right = 0, top = size, bottom = 0;
            for (unsigned y = 0; y < size; ++y)
                for (unsigned x = 0; x < size; ++x)
                    if (pixels[(size_t(y) * size + x) * 4 + 3] > 16)
                    {
                        left = std::min(left, x);
                        right = std::max(right, x + 1);
                        top = std::min(top, y);
                        bottom = std::max(bottom, y + 1);
                    }
            CHECK(left == size - right && top == size - bottom);
            CHECK(left >= size / 24 - (size >= 24 ? 1 : 0));
            stbi_image_free(pixels);
        }
    };
    auto ico = LogoIco(source), icns = LogoIcns(source), png = FitLogoIcon(source, 512);
    CHECK(ico.size() > 118 && ico[4] == 7 && ico[5] == 0);
    auto read32 = [](const std::vector<uint8_t>& bytes, size_t at, bool big) {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= uint32_t(bytes[at + i]) << (8 * (big ? 3 - i : i));
        return value;
    };
    unsigned index = 0;
    for (unsigned size : { 16u, 24u, 32u, 48u, 64u, 128u, 256u })
    {
        size_t entry = 6 + 16 * index++;
        CHECK(ico[entry] == uint8_t(size) && ico[entry + 1] == uint8_t(size));
        auto count = read32(ico, entry + 8, false), offset = read32(ico, entry + 12, false);
        CHECK(offset + count <= ico.size());
        decode(ico.data() + offset, count, size);
    }
    CHECK(std::string(icns.begin(), icns.begin() + 4) == "icns" && read32(icns, 4, true) == icns.size());
    size_t at = 8;
    for (unsigned size : { 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 32u, 64u, 256u, 512u })
    {
        auto count = read32(icns, at + 4, true);
        CHECK(at + count <= icns.size());
        decode(icns.data() + at + 8, count - 8, size);
        at += count;
    }
    CHECK(at == icns.size());
    fs::create_directories(scratch);
    for (const auto& [name, bytes] : std::vector<std::pair<std::string, std::vector<uint8_t>>>{
             { "synthetic.ico", ico }, { "synthetic.icns", icns }, { "synthetic.png", png } })
    {
        std::ofstream file(scratch / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        CHECK(file.good());
    }
    decode(png.data(), png.size(), 512);
    CHECK(FitLogoIcon({}, 512).empty() && LogoIco({}).empty() && LogoIcns({}).empty());
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    TestGames(argv[1]);
    TestUpgrade(argv[1], argv[2]);
    TestWorkFolder(argv[2]);
    TestIcons(argv[2]);
    TestShaders(argv[2]);
    for (const fs::path& path : { fs::path(argv[2]) / "missing", fs::path(argv[2]) })
    {
        bool refused = false;
        try
        {
            Sha256File(path);
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        CHECK(refused); // a missing/unreadable dependency must not become an empty freshness key
    }
    TestShaderCacheLiterals();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
