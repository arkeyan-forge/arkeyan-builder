// The VERSIONINFO of a game SkyBuild links on Windows, as windres source: the Details tab of the exe's properties.
#pragma once

#include <algorithm>
#include <string>

// `version`: game.txt's, "0.8.2". VERSIONINFO's numbers come in fours.
inline std::string VersionResource(const std::string& name, const std::string& runtime, const std::string& version)
{
    std::string parts = version;
    std::replace(parts.begin(), parts.end(), '.', ',');
    for (auto commas = std::count(parts.begin(), parts.end(), ','); commas < 3; commas++)
        parts += ",0";
    return "1 VERSIONINFO\n"
           "FILEVERSION " +
           parts +
           "\n"
           "PRODUCTVERSION " +
           parts +
           "\n"
           "FILEOS 0x40004\n"
           "FILETYPE 1\n"
           "{\n"
           "    BLOCK \"StringFileInfo\"\n"
           "    {\n"
           "        BLOCK \"040904B0\"\n"
           "        {\n"
           "            VALUE \"FileDescription\", \"" +
           name +
           "\"\n"
           "            VALUE \"FileVersion\", \"" +
           version +
           "\"\n"
           "            VALUE \"InternalName\", \"" +
           runtime +
           "\"\n"
           "            VALUE \"OriginalFilename\", \"" +
           runtime +
           ".exe\"\n"
           "            VALUE \"ProductName\", \"" +
           name +
           "\"\n"
           "            VALUE \"ProductVersion\", \"" +
           version +
           "\"\n"
           "        }\n"
           "    }\n"
           "    BLOCK \"VarFileInfo\"\n"
           "    {\n"
           "        VALUE \"Translation\", 0x409, 0x4B0\n"
           "    }\n"
           "}\n";
}
