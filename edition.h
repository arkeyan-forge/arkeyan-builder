// The edition finder (docs/tools/edition_finder.md): what a player's disc is, in a short text to paste in an issue, and
// whether it runs the code of a supported release. Ids, sizes and hashes only: no byte of the game, no path, no name.
#pragma once
#include "fingerprint.h"
#include "game_config.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class DiscFileSystem;
struct Image;

namespace skybuild
{
    struct DiscEdition
    {
        uint32_t titleId = 0, mediaId = 0, version = 0;
        std::optional<uint32_t> region;
        std::vector<std::string> textLanguages, voiceLanguages; // empty: not known
        uint64_t xexSize = 0;
        std::string xexSha256;
        uint32_t imageBase = 0;
        uint64_t imageSize = 0;
        std::string imageSha256; // empty: the executable could not be decrypted
    };

    // Reads default.xex of an open disc (or copy of the game data) and its loaded image; the kit's games name its
    // languages. nullopt and `error` when there is no readable XEX2 executable.
    // `code`, when given, receives the loaded image's code (empty when it could not be decrypted).
    std::optional<DiscEdition> ReadDiscEdition(const DiscFileSystem& disc, const std::vector<Game>& games,
        std::string& error, fingerprint::Code* code = nullptr);
    fingerprint::Code ImageCode(const Image& image);

    // "0.0.0.14" (major.minor.build.qfe).
    std::string FormatXexVersion(uint32_t version);
    // "00FF0000 (Europe and Australia)".
    std::string RegionText(uint32_t region);
    // The label of an edition known only by its region: "North America", "Europe and Australia", "Region free".
    std::string RegionLabel(std::optional<uint32_t> region);

    // How a disc relates to the kit's games.
    struct EditionMatch
    {
        const Game* game = nullptr; // the game of that title ID, if the kit has it
        int sameXex = -1; // the release whose default.xex it is
        int sameImage = -1; // else the first release whose loaded image it has
    };
    EditionMatch MatchEdition(const DiscEdition& edition, const std::vector<Game>& games);

    // The text the player pastes (ends with a newline). `tools`: "Arkeyan-Builder 0.9.5". `found`: where the disc has
    // the addresses of the game's addresses.h, when its code differs and the kit has their fingerprints.
    std::string EditionReport(const DiscEdition& edition, const std::vector<Game>& games, const std::string& tools,
        const std::vector<fingerprint::Found>* found = nullptr);
}
