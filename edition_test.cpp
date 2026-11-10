// The edition finder (edition.h): the report's text on invented editions, every game.txt's release hashes against its
// info.cpp, and with the discs (SWAPFORCE_ISO, TRAPTEAM_ISO, SSA_ISO, each optional) the loaded image of a real disc, a
// header-only edition made from it (another media ID, version and region) and a disc whose code differs.
//   ArkeyanEditionTest <repo> <scratch dir>
#include "edition.h"
#include "host/xex_title.h"
#include "sha256.h"
#include "vfs.h"
#include <image.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using namespace skybuild;
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

static uint32_t Be32(const std::vector<uint8_t>& bytes, size_t at)
{
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
}

static void Put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[at + i] = uint8_t(value >> (8 * (3 - i)));
}

static std::string Hex(uint32_t value)
{
    char text[16];
    snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

// The file offset of an optional header's data (0: absent).
static size_t OptionalHeader(const std::vector<uint8_t>& xex, uint32_t key)
{
    for (uint32_t i = 0; i < Be32(xex, 0x14); i++)
        if (Be32(xex, 0x18 + i * 8) == key)
            return Be32(xex, 0x18 + i * 8 + 4);
    return 0;
}

// Another edition's header on the same executable: media ID, version, region (unsigned by us; nothing checks it).
static void Reheader(std::vector<uint8_t>& xex, uint32_t titleId, uint32_t mediaId, uint32_t version, uint32_t region)
{
    size_t info = OptionalHeader(xex, 0x00040006);
    Put32(xex, info, mediaId);
    Put32(xex, info + 4, version);
    Put32(xex, info + 12, titleId);
    Put32(xex, Be32(xex, 0x10) + 0x178, region);
}

// A copy of the game data (a folder) holding only this default.xex: DiscFileSystem opens it like a disc.
static std::optional<DiscEdition> EditionOf(
    const fs::path& folder, const std::vector<uint8_t>& xex, const std::vector<Game>& games)
{
    fs::remove_all(folder);
    fs::create_directories(folder);
    std::ofstream(folder / "default.xex", std::ios::binary)
        .write(reinterpret_cast<const char*>(xex.data()), xex.size());
    DiscFileSystem disc;
    CHECK(disc.Open(folder.string().c_str()));
    std::string error;
    auto edition = ReadDiscEdition(disc, games, error);
    fs::remove_all(folder);
    return edition;
}

static std::vector<Game> RepoGames(const fs::path& root)
{
    std::vector<Game> games;
    for (const char* id : { "ssa", "giants", "swapforce", "trapteam", "superchargers", "imaginators" })
        games.push_back(ReadGame(id, ReadKeyValues(root / "games" / id / "game.txt")));
    return games;
}

static void TestGameFiles(const std::vector<Game>& games, const fs::path& root)
{
    for (const Game& game : games)
    {
        std::ifstream file(root / "games" / game.id / "info.cpp");
        std::string info{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        CHECK(game.titleId != 0 && info.find(Hex(game.titleId)) != std::string::npos);
        CHECK(game.imageSha256.size() == game.xexSha256.size());
        CHECK(game.releases.size() == game.xexSha256.size());
        for (size_t i = 0; i < game.imageSha256.size(); i++)
        {
            // info.cpp's release: its label, then its default.xex's hash, then its image's.
            size_t label = info.find("{ \"" + game.releases[i] + "\",");
            size_t xex = info.find("\"" + game.xexSha256[i] + "\"", label);
            size_t image = info.find("\"" + game.imageSha256[i] + "\"", xex);
            CHECK(game.imageSha256[i].size() == 64 && label != std::string::npos && xex != std::string::npos &&
                  image == info.find_first_of('"', xex + 66));
        }
    }
}

static void TestReport(const std::vector<Game>& games)
{
    const Game& swapforce = games[2];
    DiscEdition us;
    us.titleId = swapforce.titleId;
    us.mediaId = 0x12345678;
    us.version = 0x0000000E;
    us.region = 0x000000FF;
    us.textLanguages = { "en", "fr" };
    us.voiceLanguages = { "en" };
    us.xexSize = 1000;
    us.xexSha256 = std::string(64, 'a');
    us.imageBase = 0x82000000;
    us.imageSize = 4096;
    us.imageSha256 = swapforce.imageSha256[0];
    const std::string report = EditionReport(us, games, "Arkeyan-Builder 0.9.5");
    const std::string expected = "Edition report (Arkeyan-Builder 0.9.5)\n"
                                 "Game: Skylanders Swap Force (swapforce)\n"
                                 "Title ID: 415608E9\nMedia ID: 12345678\nVersion: 0.0.0.14\n"
                                 "Region: 000000FF (North America)\n"
                                 "Languages: text en, fr; voices en\n"
                                 "default.xex: 1000 bytes, SHA-256 " +
                                 us.xexSha256 + "\nLoaded image: 4096 bytes at 82000000, SHA-256 " + us.imageSha256 +
                                 "\nSupported releases:\n- USA, Australia: same game code\n- Europe: same game code\n"
                                 "Result: supported, the same game code as USA, Australia (only the disc header "
                                 "differs)\n";
    CHECK(report == expected);
    if (report != expected)
        fprintf(stderr, "%s", report.c_str());
    EditionMatch match = MatchEdition(us, games);
    CHECK(match.game == &swapforce && match.sameXex == -1 && match.sameImage == 0);

    // Other code, with the kit's fingerprints: each address and where this disc has it, then those not found.
    us.imageSha256 = std::string(64, 'c');
    const std::vector<fingerprint::Found> found = { { "GAME_HOOK_A", 0x82F00000, 0x82F00100, "sure" },
        { "Data", 0x83000000, 0, "ambiguous: 2 candidates" } };
    CHECK(EditionReport(us, games, "t", &found)
            .ends_with("Addresses on this disc: 1 of 2 found (the supported release's, then this disc's)\n"
                       "- GAME_HOOK_A 82F00000 82F00100 sure\nNot found:\n- Data 83000000 ambiguous: 2 candidates\n"));
    us.imageSha256 = swapforce.imageSha256[0];
    CHECK(EditionReport(us, games, "t", &found).find("Addresses") == std::string::npos); // same code: nothing to find

    us.imageSha256 = std::string(64, 'b');
    CHECK(EditionReport(us, games, "t").find("- Europe: different game code\nResult: not supported yet") !=
          std::string::npos);
    us.imageSha256.clear();
    CHECK(EditionReport(us, games, "t").find("Loaded image: cannot be decrypted\n") != std::string::npos);
    us.xexSha256 = swapforce.xexSha256[1];
    CHECK(EditionReport(us, games, "t").find("- Europe: this very disc\nResult: supported (Europe)\n") !=
          std::string::npos);
    us.titleId = 0x4D5307E6;
    CHECK(EditionReport(us, games, "t").find("Game: not in this kit\n") != std::string::npos);
    CHECK(EditionReport(us, games, "t").find("Result: not a game of this kit\n") != std::string::npos);
    CHECK(EditionReport(us, {}, "t").find("Result: not a game of this kit\n") != std::string::npos);

    CHECK(FormatXexVersion(0x20010A03) == "2.0.266.3");
    CHECK(RegionLabel(0xFFFFFFFF) == "Region free" && RegionLabel(0x00FF00FF) == "North America, Europe and Australia");
    CHECK(RegionLabel(std::nullopt) == "Other region" && RegionLabel(0x01000000) == "Other region");

    // Invented and cut files are refused before the loader reads them.
    CHECK(!XexImageReadable({}));
    std::vector<uint8_t> fake(0x200);
    Put32(fake, 0, 0x58455832);
    Put32(fake, 0x08, 0x200);
    Put32(fake, 0x10, 0x40);
    CHECK(!XexImageReadable(fake)); // no file format info
}

// Reads `variable`'s disc: its default.xex, or empty (and a note) when the variable is unset.
static std::vector<uint8_t> DiscXex(const char* variable)
{
    const char* path = std::getenv(variable);
    DiscFileSystem disc;
    if (path == nullptr || !disc.Open(path))
    {
        printf("SKIP the %s checks: set %s to the disc image\n", variable, variable);
        return {};
    }
    return disc.ReadAll("default.xex");
}

static void TestDiscs(const std::vector<Game>& games, const fs::path& scratch)
{
    const Game& swapforce = games[2];
    std::vector<uint8_t> xex = DiscXex("SWAPFORCE_ISO");
    if (!xex.empty())
    {
        auto edition = EditionOf(scratch / "us", xex, games);
        CHECK(edition && edition->xexSha256 == swapforce.xexSha256[0] &&
              edition->imageSha256 == swapforce.imageSha256[0]);
        CHECK(edition && edition->textLanguages.empty()); // a folder with only default.xex: no language marker
        CHECK(edition && MatchEdition(*edition, games).sameXex == 0);

        // The same executable under another header: the Europe disc's media ID and version, a North America region.
        std::vector<uint8_t> other = xex;
        Reheader(other, swapforce.titleId, 0x74FDDE06, 0x1C, 0x000000FF);
        edition = EditionOf(scratch / "header", other, games);
        CHECK(edition && edition->xexSha256 != swapforce.xexSha256[0] &&
              edition->imageSha256 == swapforce.imageSha256[1]);
        CHECK(edition && edition->mediaId == 0x74FDDE06 && edition->region == 0x000000FF);
        CHECK(edition && MatchEdition(*edition, games).sameImage == 0);
        size_t release = 9;
        CHECK(edition && FindGameByImage(games, edition->titleId, edition->imageSha256, &release) == &swapforce &&
              release == 0);
        CHECK(edition && EditionReport(*edition, games, "t").find("Result: supported, the same game code as") !=
                             std::string::npos);
        printf("Swap Force: a header-only edition is accepted (image %s...)\n",
            swapforce.imageSha256[0].substr(0, 12).c_str());

        // One byte of the executable's data changed: other code (or blocks the loader refuses), not supported.
        other[Be32(other, 0x08) + (other.size() - Be32(other, 0x08)) / 2] ^= 0x5A;
        edition = EditionOf(scratch / "damaged", other, games);
        CHECK(
            edition && edition->imageSha256 != swapforce.imageSha256[0] && MatchEdition(*edition, games).sameImage < 0);
    }

    // Trap Team's executable presented as a Swap Force edition: another game code, refused.
    std::vector<uint8_t> trap = DiscXex("TRAPTEAM_ISO");
    if (!trap.empty())
    {
        Reheader(trap, swapforce.titleId, 0x44D8BCCF, 0x0E, 0x000000FF);
        auto edition = EditionOf(scratch / "trap", trap, games);
        CHECK(edition && !edition->imageSha256.empty() && edition->imageSha256 != swapforce.imageSha256[0]);
        CHECK(edition && FindGameByImage(games, swapforce.titleId, edition->imageSha256) == nullptr);
        CHECK(edition &&
              EditionReport(*edition, games, "t").find("- USA, Australia: different game code\n") != std::string::npos);
        printf("Trap Team's code under a Swap Force header is refused\n");
    }

    // A real disc's report: its languages from the disc, its own release.
    if (const char* path = std::getenv("SSA_ISO"))
    {
        DiscFileSystem disc;
        std::string error;
        auto edition = disc.Open(path) ? ReadDiscEdition(disc, games, error) : std::nullopt;
        CHECK(edition && edition->imageSha256 == games[0].imageSha256[0] && edition->textLanguages.size() == 10);
        const std::string report = edition ? EditionReport(*edition, games, "t") : "";
        CHECK(report.find("- USA, Europe: this very disc\nResult: supported (USA, Europe)\n") != std::string::npos);
        CHECK(report.find(fs::path(path).filename().string()) == std::string::npos);
    }
}

// The loaded image of `variable`'s disc and the fingerprints of its game's addresses.h (its library's functions), or
// nothing when the disc or the library is not here.
struct GameCode
{
    fingerprint::Code code;
    std::vector<fingerprint::Print> prints;
    std::vector<std::pair<std::string, uint32_t>> addresses;
};
static std::optional<GameCode> LoadGame(const fs::path& root, const char* id, const char* variable)
{
    std::vector<uint8_t> xex = DiscXex(variable);
    const fs::path library = root / "private" / "libs" / id / "libSSARecompLib.a";
    if (xex.empty() || !fs::exists(library))
    {
        printf("SKIP the %s fingerprints: needs %s and %s\n", id, variable, "private/libs/<id>/libSSARecompLib.a");
        return std::nullopt;
    }
    Image image = XexImageReadable(xex) ? Image::ParseImage(xex.data(), xex.size()) : Image{};
    CHECK(image.data != nullptr);
    if (image.data == nullptr)
        return std::nullopt;
    GameCode game;
    game.code = ImageCode(image);
    std::ifstream lib(library, std::ios::binary), header(root / "games" / id / "addresses.h");
    auto starts =
        fingerprint::FunctionStarts({ std::istreambuf_iterator<char>(lib), std::istreambuf_iterator<char>() });
    game.addresses =
        fingerprint::ReadAddresses({ std::istreambuf_iterator<char>(header), std::istreambuf_iterator<char>() },
            game.code.base, uint32_t(game.code.words.size() * 4));
    std::vector<std::string> missing;
    game.prints = fingerprint::Make(game.code, starts, game.addresses, missing);
    CHECK(starts.size() > 10000 && !game.prints.empty());
    return game;
}

// Another build of the same code: `count` words inserted at `at` (in the code), every relative branch that crosses it
// fixed; the data references (lis pairs) are left as they are, so data addresses do not move.
static fingerprint::Code Shifted(const fingerprint::Code& code, uint32_t at, uint32_t count)
{
    fingerprint::Code shifted = code;
    const uint32_t shift = count * 4;
    auto moved = [&](uint32_t address) { return address >= at && code.IsCode(address) ? address + shift : address; };
    shifted.words.insert(shifted.words.begin() + (at - code.base) / 4, count, 0x60000000); // nop
    for (auto& [start, end] : shifted.sections)
        end += start <= at && at < end ? shift : 0, start += start > at ? shift : 0;
    for (const auto& [start, end] : code.sections)
        for (uint32_t address = start; address < end; address += 4)
        {
            uint32_t word = code.Word(address), op = word >> 26;
            if ((op != 18 && op != 16) || (word & 2)) // b/bl, bc: relative only
                continue;
            uint32_t mask = op == 18 ? 0x03FFFFFC : 0xFFFC, sign = op == 18 ? 0x02000000 : 0x8000;
            uint32_t offset = word & mask, target = address + (offset & sign ? offset - (sign << 1) : offset);
            uint32_t from = moved(address), to = moved(target);
            if (from - address == to - target)
                continue;
            uint32_t fixed = (word & ~mask) | ((to - from) & mask);
            shifted.words[(from - shifted.base) / 4] = fixed;
        }
    return shifted;
}

static void TestFingerprints(const fs::path& root, const fs::path& scratch)
{
    auto swapforce = LoadGame(root, "swapforce", "SWAPFORCE_ISO");
    if (!swapforce)
        return;
    const auto& prints = swapforce->prints;
    CHECK(fingerprint::Write(fingerprint::Read(fingerprint::Write(prints))) == fingerprint::Write(prints));
    CHECK(fingerprint::Read(fingerprint::Write(prints)).size() == prints.size());
    CHECK(prints.size() == swapforce->addresses.size()); // every function hook and data address got one

    unsigned same = 0;
    for (const auto& found : fingerprint::Find(swapforce->code, prints))
        same += found.theirs == found.ours;
    CHECK(same == prints.size());

    // A shifted build: 64 words inserted in the middle of the first code section.
    const auto& [first, last] = swapforce->code.sections.front();
    const uint32_t at = (first + (last - first) / 2) & ~3u, shift = 64 * 4;
    const fingerprint::Code shifted = Shifted(swapforce->code, at, 64);
    unsigned moved = 0, kept = 0, wrong = 0;
    for (const auto& found : fingerprint::Find(shifted, prints))
    {
        const bool isData = !swapforce->code.IsCode(found.ours);
        const uint32_t expected = !isData && found.ours >= at ? found.ours + shift : found.ours;
        if (found.theirs == expected)
            (expected != found.ours ? moved : kept)++;
        else
        {
            wrong++;
            fprintf(stderr, "shifted: %s %08X -> %08X (%s), expected %08X\n", found.name.c_str(), found.ours,
                found.theirs, found.confidence.c_str(), expected);
        }
    }
    CHECK(wrong == 0 && moved > 0 && kept > 0);
    printf("Swap Force fingerprints: %zu, all found in place; in a shifted copy %u moved and %u kept, all found\n",
        prints.size(), moved, kept);

    // The report of a disc of other code (Trap Team's executable under a Swap Force header) lists every address.
    std::vector<uint8_t> trap = DiscXex("TRAPTEAM_ISO");
    if (!trap.empty())
    {
        Reheader(trap, 0x415608E9, 0x44D8BCCF, 0x0E, 0x000000FF);
        auto edition = EditionOf(scratch / "trap-edition", trap, RepoGames(root));
        Image image = XexImageReadable(trap) ? Image::ParseImage(trap.data(), trap.size()) : Image{};
        CHECK(edition && image.data != nullptr);
        if (edition && image.data != nullptr)
        {
            auto located = fingerprint::Find(ImageCode(image), prints);
            const std::string report = EditionReport(*edition, RepoGames(root), "t", &located);
            size_t line = report.find("Addresses on this disc: ");
            CHECK(line != std::string::npos &&
                  report.find(" of " + std::to_string(prints.size()) + " found", line) != std::string::npos);
            // Those found are Trap Team's own (games/trapteam/addresses.h), where it names them.
            std::ifstream header(root / "games" / "trapteam" / "addresses.h");
            std::map<std::string, uint32_t> own;
            for (const auto& [name, address] :
                fingerprint::ReadAddresses({ std::istreambuf_iterator<char>(header), std::istreambuf_iterator<char>() },
                    0x82000000, 0x10000000))
                own[name] = address;
            unsigned agree = 0, differ = 0;
            for (const auto& address : located)
                if (address.theirs != 0 && own.count(address.name))
                    (own[address.name] == address.theirs ? agree : differ)++;
            CHECK(differ == 0);
            printf("Trap Team's code as a Swap Force edition: %s  %u agree with Trap Team's addresses.h, %u differ\n",
                report.substr(line, report.find('\n', line) - line + 1).c_str(), agree, differ);
        }
    }

    // Giants' D3D functions that Swap Force has unchanged (games/swapforce/addresses.h notes them), found by Giants'
    // fingerprints in Swap Force's code: another real build.
    auto giants = LoadGame(root, "giants", "GIANTS_ISO");
    if (!giants)
        return;
    std::map<std::string, uint32_t> found;
    for (const auto& address : fingerprint::Find(swapforce->code, giants->prints))
        found[address.name] = address.theirs;
    // (Its NextRingSegment, KickOff, Swap... changed between the two D3D builds: not found, rightly.)
    CHECK(found["GAME_HOOK_D3DVertexShader_Init"] == 0x82F527F8);
    CHECK(found["GAME_HOOK_D3DPixelShader_Init"] == 0x82F51A10);
    unsigned agree = 0, differ = 0;
    std::map<std::string, uint32_t> own(swapforce->addresses.begin(), swapforce->addresses.end());
    for (const auto& [name, address] : found)
        if (address != 0 && own.count(name))
            (own[name] == address ? agree : differ)++;
    CHECK(differ == 0 && agree >= 10);
    printf("Giants' fingerprints in Swap Force's code: %u agree with Swap Force's addresses.h, %u differ\n", agree,
        differ);
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: ArkeyanEditionTest <repo> <scratch dir>\n");
        return 2;
    }
    const fs::path root = argv[1], scratch = fs::path(argv[2]) / "edition";
    std::vector<Game> games = RepoGames(root);
    TestGameFiles(games, root);
    TestReport(games);
    TestDiscs(games, scratch);
    TestFingerprints(root, scratch);
    fs::remove_all(scratch);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
