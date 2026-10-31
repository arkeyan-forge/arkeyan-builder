// The game metadata shipped in config/<id>/game.txt; shared by SkyBuild and its disc-free tests.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace skybuild
{
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
        std::string name; // "Skylanders: Giants"
        std::string runtime; // the built game's name: SSARecomp, SGRecomp
        std::string toml; // XenonRecomp's config, in config/<id>/
        std::string shaders; // where the shaders come from: "lsa" (shaders.lsa, effects.lsa and the XEX image)
        // or "igz:<archive path>" (Giants, Swap Force), "igz:all" (Trap Team: IGA v11 / IGZ v8)
        std::vector<std::string> xexSha256; // the accepted default.xex files
        // The movie pass (AddMovieShaders); empty: no portal, no presses, it ends 8 s after the first new shader.
        std::string moviePortal; // SKY_PORTAL for the run ("emulated": an empty emulated portal)
        std::string moviePress; // SKY_PRESS for the run (presses through the menus)
        unsigned movieShaders = 0; // the run ends once this many new shaders are dumped
    };

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
        return { id, values["name"], values["runtime"], values["toml"], values["shaders"],
            SplitList(values["xex_sha256"]), values["movie_portal"], values["movie_press"],
            unsigned(strtoul(values["movie_shaders"].c_str(), nullptr, 10)) };
    }

    inline const Game* FindGame(const std::vector<Game>& games, const std::string& sha)
    {
        for (const Game& game : games)
            if (std::find(game.xexSha256.begin(), game.xexSha256.end(), sha) != game.xexSha256.end())
                return &game;
        return nullptr;
    }
}
