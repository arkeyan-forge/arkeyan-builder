#include "edition.h"
#include "host/art/alchemy_archive.h"
#include "host/art/disc_languages.h"
#include "host/xex_title.h"
#include "sha256.h"
#include "vfs.h"
#include <cstdio>
#include <image.h>
#include <utility>

namespace skybuild
{
    fingerprint::Code ImageCode(const Image& image)
    {
        std::vector<std::pair<uint32_t, uint32_t>> sections;
        for (const Section& section : image.sections)
            if (section.flags & SectionFlags_Code)
                sections.emplace_back(uint32_t(section.base), uint32_t(section.base + section.size));
        return fingerprint::FromBytes(uint32_t(image.base), image.data.get(), image.size, std::move(sections));
    }

    std::optional<DiscEdition> ReadDiscEdition(
        const DiscFileSystem& disc, const std::vector<Game>& games, std::string& error, fingerprint::Code* code)
    {
        const DiscFileSystem::Entry* entry = disc.Find("default.xex");
        if (entry == nullptr || entry->directory)
        {
            error = "the disc has no default.xex: it is not an Xbox 360 game disc";
            return {};
        }
        std::vector<uint8_t> xex = disc.ReadAll("default.xex");
        auto execution = ReadExecutionInfo(xex);
        if (xex.size() != entry->size || !execution)
        {
            error = "default.xex is cut short or is not an Xbox 360 executable: copy the disc again";
            return {};
        }
        DiscEdition edition;
        edition.titleId = execution->titleId;
        edition.mediaId = execution->mediaId;
        edition.version = execution->version;
        edition.region = ReadRegion(xex);
        edition.xexSize = xex.size();
        edition.xexSha256 = Sha256Bytes(xex.data(), xex.size());
        Image image = XexImageReadable(xex) ? Image::ParseImage(xex.data(), xex.size()) : Image{};
        if (image.data != nullptr)
        {
            edition.imageBase = uint32_t(image.base);
            edition.imageSize = image.size;
            edition.imageSha256 = Sha256Bytes(image.data.get(), image.size);
            if (code != nullptr)
                *code = ImageCode(image);
        }

        std::string id;
        for (const Game& game : games)
            if (game.titleId == edition.titleId)
                id = game.id;
        std::vector<std::string> names;
        for (const auto& file : disc.Files())
            names.push_back(file.first);
        auto archiveNames = [&disc](const std::string& path) {
            std::vector<std::string> entries;
            const DiscFileSystem::Entry* file = disc.Find(path);
            if (file == nullptr || file->directory)
                return entries;
            auto archive = art::alchemy::IgaArchive::Open(
                [&](uint64_t offset, size_t size, uint8_t* out) {
                    return disc.Read(*file, offset, out, uint32_t(size)) == size;
                },
                file->size);
            for (size_t i = 0; archive && i < archive->Count(); i++)
                entries.push_back(archive->Name(i));
            return entries;
        };
        art::DiscLanguages languages = art::LanguagesOnDisc(id, names, archiveNames);
        edition.textLanguages = std::move(languages.text);
        edition.voiceLanguages = std::move(languages.voice);
        return edition;
    }

    std::string FormatXexVersion(uint32_t version)
    {
        char text[32];
        snprintf(text, sizeof(text), "%u.%u.%u.%u", version >> 28, (version >> 24) & 0xF, (version >> 8) & 0xFFFF,
            version & 0xFF);
        return text;
    }

    namespace
    {
        constexpr std::pair<uint32_t, const char*> Areas[] = { { 0x000000FF, "North America" },
            { 0x0000FF00, "Japan and Asia" }, { 0x00FF0000, "Europe and Australia" } };

        std::string Hex8(uint32_t value)
        {
            char text[9];
            snprintf(text, sizeof(text), "%08X", value);
            return text;
        }

        std::string Join(const std::vector<std::string>& items)
        {
            std::string text;
            for (const std::string& item : items)
                text += (text.empty() ? "" : ", ") + item;
            return text.empty() ? "not known" : text;
        }
    }

    std::string RegionLabel(std::optional<uint32_t> region)
    {
        if (!region)
            return "Other region";
        if (*region == 0xFFFFFFFF)
            return "Region free";
        std::string names;
        for (const auto& [mask, name] : Areas)
            if (*region & mask)
                names += (names.empty() ? "" : ", ") + std::string(name);
        return names.empty() ? "Other region" : names;
    }

    std::string RegionText(uint32_t region) { return Hex8(region) + " (" + RegionLabel(region) + ")"; }

    EditionMatch MatchEdition(const DiscEdition& edition, const std::vector<Game>& games)
    {
        EditionMatch match;
        for (const Game& game : games)
            if (game.titleId == edition.titleId)
                match.game = &game;
        if (match.game == nullptr)
            return match;
        const Game& game = *match.game;
        for (size_t i = 0; i < game.xexSha256.size(); i++)
            if (game.xexSha256[i] == edition.xexSha256)
                match.sameXex = int(i);
        for (size_t i = 0; i < game.imageSha256.size() && match.sameImage < 0; i++)
            if (!edition.imageSha256.empty() && game.imageSha256[i] == edition.imageSha256)
                match.sameImage = int(i);
        return match;
    }

    std::string EditionReport(const DiscEdition& edition, const std::vector<Game>& games, const std::string& tools,
        const std::vector<fingerprint::Found>* found)
    {
        const EditionMatch match = MatchEdition(edition, games);
        auto label = [&](size_t i) {
            return i < match.game->releases.size() ? match.game->releases[i] : "release " + std::to_string(i + 1);
        };
        std::string text = "Edition report (" + tools + ")\n";
        text +=
            "Game: " + (match.game != nullptr ? match.game->name + " (" + match.game->id + ")" : "not in this kit") +
            "\n";
        text += "Title ID: " + Hex8(edition.titleId) + "\nMedia ID: " + Hex8(edition.mediaId) +
                "\nVersion: " + FormatXexVersion(edition.version) + "\n";
        text += "Region: " + (edition.region ? RegionText(*edition.region) : std::string("not known")) + "\n";
        text += "Languages: text " + Join(edition.textLanguages) + "; voices " + Join(edition.voiceLanguages) + "\n";
        text += "default.xex: " + std::to_string(edition.xexSize) + " bytes, SHA-256 " + edition.xexSha256 + "\n";
        if (edition.imageSha256.empty())
            text += "Loaded image: cannot be decrypted\n";
        else
            text += "Loaded image: " + std::to_string(edition.imageSize) + " bytes at " + Hex8(edition.imageBase) +
                    ", SHA-256 " + edition.imageSha256 + "\n";
        if (match.game == nullptr)
            return text + "Result: not a game of this kit\n";
        text += "Supported releases:\n";
        for (size_t i = 0; i < match.game->xexSha256.size(); i++)
        {
            const bool known = i < match.game->imageSha256.size() && !match.game->imageSha256[i].empty();
            const bool same = known && match.game->imageSha256[i] == edition.imageSha256;
            text += "- " + label(i) + ": " +
                    (int(i) == match.sameXex ? "this very disc"
                        : same               ? "same game code"
                        : known              ? "different game code"
                                             : "game code not known") +
                    "\n";
        }
        if (match.sameXex >= 0)
            text += "Result: supported (" + label(size_t(match.sameXex)) + ")\n";
        else if (match.sameImage >= 0)
            text += "Result: supported, the same game code as " + label(size_t(match.sameImage)) +
                    " (only the disc header differs)\n";
        else
            text += "Result: not supported yet: the game code differs from every supported release\n";
        if (found != nullptr && match.sameXex < 0 && match.sameImage < 0)
        {
            size_t count = 0;
            std::string missing;
            for (const fingerprint::Found& address : *found)
                count += address.theirs != 0;
            text += "Addresses on this disc: " + std::to_string(count) + " of " + std::to_string(found->size()) +
                    " found (the supported release's, then this disc's)\n";
            for (const fingerprint::Found& address : *found)
            {
                if (address.theirs != 0)
                    text += "- " + address.name + " " + Hex8(address.ours) + " " + Hex8(address.theirs) + " " +
                            address.confidence + "\n";
                else
                    missing += "- " + address.name + " " + Hex8(address.ours) + " " + address.confidence + "\n";
            }
            if (!missing.empty())
                text += "Not found:\n" + missing;
        }
        return text;
    }
}
