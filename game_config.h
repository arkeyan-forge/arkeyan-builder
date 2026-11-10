// The game metadata shipped in config/<id>/game.txt; shared by Arkeyan-Builder and its disc-free tests.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace skybuild
{
    // Existing stamps, objects and shader caches remain at their original paths.
    // Prefer the legacy folder even when both exist; no rename or migration is needed.
    inline std::filesystem::path WorkFolder(const std::filesystem::path& output)
    {
        std::error_code error;
        auto legacy = output / "ssabuild-work";
        return std::filesystem::is_directory(legacy, error) ? legacy : output / "arkeyan-work";
    }

    // The Exec value has two quoting layers: desktop string escapes, then argument quoting.
    inline std::string DesktopArgument(const std::string& text)
    {
        std::string quoted = "\"";
        for (char c : text)
        {
            if (c == '\\' || c == '"' || c == '`' || c == '$')
                quoted += '\\';
            quoted += c == '%' ? "%%" : std::string(1, c);
        }
        quoted += '"';
        std::string value;
        for (char c : quoted)
            value += c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\r' ? "\\r" : std::string(1, c);
        return value;
    }

    inline std::string GameDesktopEntry(const std::string& name, const std::string& executable, const std::string& icon)
    {
        return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=" + DesktopArgument(executable) +
               "\nIcon=" + icon + "\nTerminal=false\nCategories=Game;\n";
    }

    // key=value lines.
    inline std::map<std::string, std::string> ReadKeyValues(const std::filesystem::path& path)
    {
        std::map<std::string, std::string> values;
        std::ifstream file(path);
        for (std::string line; std::getline(file, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            size_t equals = line.find('=');
            if (equals != std::string::npos && line[0] != '#')
                values[line.substr(0, equals)] = line.substr(equals + 1);
        }
        return values;
    }

    // One game of the kit: config/<id>/game.txt.
    struct Game
    {
        std::string id; // "ssa", "giants": the kit's folders (config, commands, lib, include, res) are named so
        std::string name; // "Skylanders Giants"
        std::string runtime; // the built game's filename, including spaces and apostrophes
        std::string toml; // XenonRecomp's config, in config/<id>/
        std::string shaders; // where the shaders come from: "lsa" (shaders.lsa, effects.lsa and the XEX image)
        // or "igz:<archive path>" (Giants, Swap Force), "igz:all" (Trap Team: IGA v11 / IGZ v8)
        std::vector<std::string> xexSha256; // the accepted default.xex files
        // Each of those releases' loaded image (the decrypted, decompressed basefile; same order, an empty item: not
        // known) and its label ("USA, Australia"). Another edition whose loaded image is one of these runs the same code:
        // only its XEX header differs (docs/tools/edition_finder.md).
        std::vector<std::string> imageSha256;
        std::vector<std::string> releases;
        uint32_t titleId = 0;
        // The movie pass (AddMovieShaders); empty: no portal, no presses, and with no count it ends 8 s after the first new
        // shader.
        std::string moviePortal; // SKY_PORTAL for the run ("emulated": an empty emulated portal)
        std::string moviePress; // SKY_PRESS for the run (presses through the menus)
        unsigned movieShaders = 0; // the run ends once the game has this many run-time shaders
    };

    // What to tell the player when Windows would not start one of the kit's programs: an antivirus's false alarm stops the
    // install halfway, and only the player can allow the file. `code`: the Windows error, 0: none.
    inline std::string StartErrorHint(unsigned long code, const std::string& program)
    {
        switch (code)
        {
        case 0: return {};
        case 225: // ERROR_VIRUS_INFECTED
        case 226: // ERROR_VIRUS_DELETED
            return ": Windows Security blocked " + program +
                   ", a program of the game kit. Allow it in Windows Security > Protection history, then install again";
        case 2: // ERROR_FILE_NOT_FOUND
        case 3: // ERROR_PATH_NOT_FOUND
            return ": " + program +
                   " is missing from the game kit. If your antivirus removed it, restore it, then install again";
        case 5: // ERROR_ACCESS_DENIED
            return ": Windows refused to start " + program +
                   ", a program of the game kit. Your antivirus may be blocking it";
        default: return " (Windows error " + std::to_string(code) + ")";
        }
    }

    // movies/done.txt holds how many shaders the pass found. Fewer than the game has is not done: a pass that ended early
    // leaves a game whose missing shaders' draws are skipped (Giants: a black picture on Windows).
    inline bool MoviePassDone(const std::string& stamp, unsigned expected)
    {
        unsigned count = 0;
        const char* end = stamp.data() + stamp.size();
        auto parsed = std::from_chars(stamp.data(), end, count);
        if (parsed.ec != std::errc{} || parsed.ptr == stamp.data())
            return false;
        for (const char* at = parsed.ptr; at != end; ++at)
            if (*at != '\r' && *at != '\n')
                return false;
        return count > 0 && count >= expected;
    }

    inline std::string LegacyRuntime(const std::string& id)
    {
        if (id == "ssa")
            return "SSARecomp";
        if (id == "giants")
            return "SGRecomp";
        if (id == "swapforce")
            return "SSFRecomp";
        if (id == "trapteam")
            return "TTRecomp";
        return {};
    }

    // Call only after linking and signing succeeded. An interrupted rebuild keeps the old install.
    inline void RemoveLegacyRuntime(const Game& game, const std::filesystem::path& folder, const std::string& suffix)
    {
        const auto legacy = LegacyRuntime(game.id);
        if (legacy.empty() || legacy == game.runtime)
            return;
        auto current = folder / (game.runtime + suffix);
        if (suffix == ".app")
            current /= std::filesystem::path("Contents") / "MacOS" / game.runtime;
        if (!std::filesystem::is_regular_file(current))
            return;
        std::filesystem::remove_all(folder / (legacy + suffix));
        if (suffix == ".app")
            std::filesystem::remove(folder / legacy); // old command-line symlink beside the bundle
    }

    inline std::vector<std::string> SplitList(const std::string& text)
    {
        std::vector<std::string> items;
        std::stringstream stream(text);
        for (std::string item; std::getline(stream, item, ',');)
            if (!item.empty())
                items.push_back(item);
        return items;
    }

    inline Game ReadGame(const std::string& id, std::map<std::string, std::string> values)
    {
        Game game;
        game.id = id;
        game.name = values["name"];
        game.runtime = values["runtime"];
        game.toml = values["toml"];
        game.shaders = values["shaders"];
        game.xexSha256 = SplitList(values["xex_sha256"]);
        game.imageSha256 = SplitList(values["image_sha256"]);
        std::stringstream labels(values["releases"]);
        for (std::string label; std::getline(labels, label, ';');)
            game.releases.push_back(label);
        game.titleId = uint32_t(strtoul(values["title_id"].c_str(), nullptr, 16));
        game.moviePortal = values["movie_portal"];
        game.moviePress = values["movie_press"];
        game.movieShaders = unsigned(strtoul(values["movie_shaders"].c_str(), nullptr, 10));
        return game;
    }

    inline const Game* FindGame(const std::vector<Game>& games, const std::string& sha)
    {
        for (const Game& game : games)
            if (std::find(game.xexSha256.begin(), game.xexSha256.end(), sha) != game.xexSha256.end())
                return &game;
        return nullptr;
    }

    // The game a disc of another edition belongs to: its title ID and a supported release's loaded image. `release`
    // receives that release's index.
    inline const Game* FindGameByImage(
        const std::vector<Game>& games, uint32_t titleId, const std::string& imageSha, size_t* release = nullptr)
    {
        for (const Game& game : games)
            for (size_t i = 0; i < game.imageSha256.size(); i++)
                if (game.titleId == titleId && !imageSha.empty() && game.imageSha256[i] == imageSha)
                {
                    if (release != nullptr)
                        *release = i;
                    return &game;
                }
        return nullptr;
    }
}
