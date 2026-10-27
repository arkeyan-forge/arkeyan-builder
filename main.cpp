// SSABuild: builds the game on the player's machine from their disc and the player kit (docs/tools/ssabuild.md).
//
//   SSABuild --iso <iso or copied game data folder> --out <game dir> [--kit <kit dir>] [--jobs N] [--toolchain <dir>]
//
// The kit holds several games (kit.txt: games=ssa,giants). The disc's default.xex, by its SHA-256, says which one this
// is (config/<id>/game.txt); everything below then comes from that game's part of the kit.
//
// Steps: the compiler (llvm-mingw on Windows, zig's clang on Linux: downloaded and checked; Xcode's clang on macOS),
// the disc's files
// (default.xex, its loaded image, the shader archives), the game code (XenonRecomp, a separate program of the kit), the
// shaders (XenosRecomp, likewise), compiling and linking against the kit's prebuilt runtime, then a short headless run
// of the new game that saves the shaders it builds at run time (Bink movies) and a rebuild of the shader cache with
// them. Work files stay in <game dir>/ssabuild-work, so a rerun only redoes what changed.
#include "host/collection/datadir.h"
#include "host/xex_title.h"
#include "igz_shaders.h"
#include "process.h"
#include "sha256.h"
#include "vfs.h"
#include <image.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace
{
    // The compiler SSABuild downloads: the one the kit's runtime was built with (tools/make_kit.sh), pinned by SHA-256.
    struct Toolchain
    {
        const char* name;
        const char* folder;  // the archive's top folder
        const char* url;
        const char* sha256;
        uint64_t size;
        unsigned extractedMB;
        const char* compiler; // in the folder
    };
#ifdef _WIN32
    constexpr Toolchain Compiler = { "llvm-mingw 20260922", "llvm-mingw-20260922-ucrt-x86_64",
        "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip",
        "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666", 190725905, 750,
        "bin/x86_64-w64-mingw32-clang++.exe" };
    constexpr const char* Exe = ".exe";
#elif defined(__linux__)
    // zig's clang with its own libc++ and glibc stubs: no system compiler, headers or -dev package is needed, and
    // the game targets glibc 2.34 whatever the distribution (docs/tools/ssabuild.md). The kit records the same version.
    constexpr const char* ZigVersion = "0.17.0";
    constexpr Toolchain Compiler = { "zig 0.17.0", "zig-x86_64-linux-0.17.0",
        "https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz",
        "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026", 57332648, 410, "zig" };
    constexpr const char* Exe = "";
#else
    constexpr const char* Exe = "";
#endif

    std::mutex g_outputMutex;

    void Emit(const std::string& kind, const std::string& text)
    {
        std::string line = text;
        std::replace(line.begin(), line.end(), '\n', ' ');
        std::replace(line.begin(), line.end(), '\r', ' ');
        std::lock_guard lock(g_outputMutex);
        fprintf(stdout, "@%s%s%s\n", kind.c_str(), line.empty() ? "" : " ", line.c_str());
        fflush(stdout);
    }

    void Log(const std::string& text) { Emit("log", text); }
    // Each step's duration goes to the log when the next step (or the end) comes: "timing <id> <seconds> s".
    std::string g_stepId;
    std::chrono::steady_clock::time_point g_stepStart;

    void EndStep()
    {
        if (!g_stepId.empty())
        {
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_stepStart);
            Log("timing " + g_stepId + " " + std::to_string(seconds.count()) + " s");
        }
        g_stepId.clear();
    }

    void Step(const char* id, const char* title)
    {
        EndStep();
        g_stepId = id;
        g_stepStart = std::chrono::steady_clock::now();
        Emit("step", std::string(id) + " " + title);
    }
    void Progress(uint64_t done, uint64_t total) { Emit("progress", std::to_string(done) + " " + std::to_string(total)); }

    struct Failure
    {
        std::string message;
    };

    [[noreturn]] void Fail(const std::string& message)
    {
        throw Failure{ message };
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }

    void WriteText(const fs::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        if (!file)
            Fail("cannot write " + Slashes(path));
    }

    void WriteBytes(const fs::path& path, const void* data, size_t size)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), std::streamsize(size));
        if (!file)
            Fail("cannot write " + Slashes(path));
    }

    // The last lines of a tool's log, as @log lines.
    void LogTail(const fs::path& log, size_t lines = 25)
    {
        std::vector<std::string> all;
        std::ifstream file(log);
        for (std::string line; std::getline(file, line);)
            all.push_back(line);
        for (size_t i = all.size() > lines ? all.size() - lines : 0; i < all.size(); i++)
            Log(all[i]);
    }

    // A digest of the content of a file or of every file under a folder (names and bytes, in name order): what decides
    // whether a result is still valid, where a time or a path would change with each unpacked kit.
    std::string ContentDigest(const fs::path& path)
    {
        std::error_code error;
        if (fs::is_regular_file(path, error))
            return Sha256File(path);
        std::map<std::string, std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(path, error))
            if (entry.is_regular_file(error))
                files[Slashes(fs::relative(entry.path(), path))] = Sha256File(entry.path());
        std::string text;
        for (const auto& [name, sha] : files)
            text += name + "=" + sha + "\n";
        return Sha256Text(text);
    }

    // Copies when the content differs, so unchanged files keep their time (and their objects stay valid).
    bool CopyIfDifferent(const fs::path& from, const fs::path& to)
    {
        std::error_code error;
        if (fs::exists(to, error) && fs::file_size(from) == fs::file_size(to, error) && ReadText(from) == ReadText(to))
            return false;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing);
        return true;
    }

    // key=value lines.
    std::map<std::string, std::string> ReadKeyValues(const fs::path& path)
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

    struct Options
    {
        fs::path iso;
        fs::path out;
        fs::path kit;
        fs::path toolchain;
        unsigned jobs = 0;
    };

    // One game of the kit: config/<id>/game.txt.
    struct Game
    {
        std::string id;                  // "ssa", "giants": the kit's folders (config, commands, lib, include, res) are named so
        std::string name;                // "Skylanders: Giants"
        std::string runtime;             // the built game's name: SSARecomp, SGRecomp
        std::string toml;                // XenonRecomp's config, in config/<id>/
        std::string shaders;             // where the shaders come from: "lsa" (shaders.lsa, effects.lsa and the XEX image)
                                         // or "igz:<path of the archive>" (IGA v8 / IGZ v6 or IGA v10 / IGZ v7, extracted here)
        std::vector<std::string> xexSha256; // the accepted default.xex files
        // The movie pass (AddMovieShaders); empty: no portal, no presses, it ends 8 s after the first new shader.
        std::string moviePortal;         // SSA_PORTAL for the run ("emulated": an empty emulated portal)
        std::string moviePress;          // SSA_PRESS for the run (presses through the menus)
        unsigned movieShaders = 0;       // the run ends once this many new shaders are dumped
    };

    struct Build
    {
        Options options;
        Game game;
        fs::path work;
        std::map<std::string, std::string> kit; // kit.txt
        std::string platform;                   // windows-x64, macos-arm64, linux-x64
        std::map<std::string, std::string> vars; // ${NAME} in the command templates
        fs::path exe;                           // the built game
        bool macos = false;                     // the kit's platform
        bool cross = false;                     // a Windows kit driven from another OS (a developer check)
    };

    std::vector<std::string> SplitList(const std::string& text)
    {
        std::vector<std::string> items;
        std::stringstream stream(text);
        for (std::string item; std::getline(stream, item, ',');)
            if (!item.empty())
                items.push_back(item);
        return items;
    }

    // Per-game folder of a kit part: <kit>/<part>/<game id>/.
    fs::path GameDir(const Build& build, const char* part) { return build.options.kit / part / build.game.id; }

    // A command line as the freshness key sees it: the kit's folder (a new one for every update) written as <kit>.
    std::string CommandText(const Build& build, const std::vector<std::string>& args)
    {
        const std::string kit = Slashes(build.options.kit);
        std::string text;
        for (std::string arg : args)
        {
            for (size_t at = arg.find(kit); at != std::string::npos; at = arg.find(kit, at + 5))
                arg.replace(at, kit.size(), "<kit>");
            text += arg + "\n";
        }
        return text;
    }

    fs::file_time_type TimeOf(const fs::path& path)
    {
        std::error_code error;
        auto time = fs::last_write_time(path, error);
        return error ? fs::file_time_type::min() : time;
    }

    // One argument per line, ${NAME} replaced; a line that is exactly ${OBJECTS} becomes the object list.
    std::vector<std::string> ExpandTemplate(const Build& build, const char* name, const std::vector<std::string>& objects)
    {
        std::vector<std::string> args;
        std::ifstream file(GameDir(build, "commands") / name);
        if (!file)
            Fail("the kit has no commands/" + build.game.id + "/" + name);
        for (std::string line; std::getline(file, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            if (line == "${OBJECTS}")
            {
                args.insert(args.end(), objects.begin(), objects.end());
                continue;
            }
            for (const auto& [key, value] : build.vars)
            {
                std::string token = "${" + key + "}";
                for (size_t at = line.find(token); at != std::string::npos; at = line.find(token, at + value.size()))
                    line.replace(at, token.size(), value);
            }
            if (line.find("${") != std::string::npos)
                Fail(std::string("unknown variable in commands/") + name + ": " + line);
            args.push_back(line);
        }
        return args;
    }

    // Arguments after the program go through a response file: no command line length limit, and paths with spaces or
    // accents are passed as they are (clang reads it as UTF-8, GNU quoting). zig's driver subcommand (zig c++, Linux)
    // stays on the command line.
    std::vector<std::string> WithResponseFile(const std::vector<std::string>& args, const fs::path& rsp)
    {
        size_t first = args.size() > 1 && (args[1] == "c++" || args[1] == "cc") ? 2 : 1;
        std::string text;
        for (size_t i = first; i < args.size(); i++)
        {
            std::string quoted = "\"";
            for (char c : args[i])
            {
                if (c == '"' || c == '\\')
                    quoted += '\\';
                quoted += c;
            }
            text += quoted + "\"\n";
        }
        if (ReadText(rsp) != text)
            WriteText(rsp, text);
        std::vector<std::string> command(args.begin(), args.begin() + std::ptrdiff_t(first));
        command.push_back("@" + Slashes(rsp));
        return command;
    }

    // --- toolchain -------------------------------------------------------------------------------------------------

#if defined(_WIN32) || defined(__linux__)
    // Downloads, checks and extracts Compiler into the cache once; returns its folder.
    fs::path DownloadToolchain(Build& build, fs::path cache)
    {
        auto hasCompiler = [](const fs::path& root) { return fs::exists(root / Compiler.compiler); };
        fs::path root = cache / Compiler.folder;
        fs::path ready = root / "ssabuild-ready.txt";
        if (hasCompiler(root) && fs::exists(ready))
            return root;
        fs::create_directories(cache);
        std::string url = Compiler.url;
        fs::path archive = cache / url.substr(url.rfind('/') + 1);
        if (!fs::exists(archive) || fs::file_size(archive) != Compiler.size)
        {
            Log("downloading " + url + " (" + std::to_string(Compiler.size >> 20) + " MB)");
            fs::path part = archive;
            part += ".part";
            fs::remove(part);
            RunOptions run;
            run.cwd = cache;
            run.log = build.work / "logs" / "download.log";
            run.poll = [&]()
            {
                std::error_code error;
                uint64_t size = fs::exists(part, error) ? fs::file_size(part, error) : 0;
                Progress(size >> 20, Compiler.size >> 20);
                return false;
            };
#ifdef _WIN32
            fs::path curl = PathFromUtf8(GetEnv("SystemRoot")) / "System32" / "curl.exe";
            int result = Run({ fs::exists(curl) ? Slashes(curl) : "curl.exe", "-fL", "--retry", "3",
                "--retry-delay", "5", "-o", Slashes(part.filename()), url }, run);
#else
            // curl, else wget (a desktop has one of them; neither needs root).
            int result = Run({ "curl", "-fL", "--retry", "3", "--retry-delay", "5", "-o", part.filename().string(), url },
                run);
            if (result == -1 || result == 127)
                result = Run({ "wget", "-q", "--tries=3", "-O", part.filename().string(), url }, run);
#endif
            if (result != 0)
            {
                LogTail(run.log, 10);
                Fail(std::string("could not download the compiler (") + Compiler.name +
                    "): check the internet connection" + (result == -1 ? " (no curl or wget found)" : ""));
            }
            fs::rename(part, archive);
        }
        Log("checking " + Slashes(archive.filename()));
        std::string sha = Sha256File(archive);
        if (sha != Compiler.sha256)
        {
            fs::remove(archive);
            Fail("the downloaded compiler does not match its SHA-256 (" + sha + "); it was deleted, try again");
        }
        Log("extracting the compiler (about " + std::to_string(Compiler.extractedMB) + " MB)");
        fs::path staging = cache / "extracting";
        fs::remove_all(staging);
        fs::remove_all(root);
        fs::create_directories(staging);
        RunOptions run;
        run.cwd = staging;
        run.log = build.work / "logs" / "extract.log";
#ifdef _WIN32
        fs::path tar = PathFromUtf8(GetEnv("SystemRoot")) / "System32" / "tar.exe";
        int result = Run({ fs::exists(tar) ? Slashes(tar) : "tar.exe", "-xf", Slashes(archive) }, run);
#else
        int result = Run({ "tar", "-xJf", archive.string() }, run);
#endif
        if (result != 0 || !hasCompiler(staging / Compiler.folder))
        {
            LogTail(run.log, 10);
            Fail("could not extract " + Slashes(archive));
        }
        fs::rename(staging / Compiler.folder, root);
        fs::remove_all(staging);
        WriteText(ready, std::string(Compiler.sha256) + "\n");
        std::error_code error;
        fs::remove(archive, error); // the extracted copy no longer needs it
        return root;
    }
#endif

    void PrepareToolchain(Build& build)
    {
        Step("toolchain", "Preparing the compiler");
#ifdef _WIN32
        fs::path cache = build.options.toolchain;
        fs::path root;
        if (!cache.empty() && fs::exists(cache / Compiler.compiler))
            root = cache;
        else
        {
            if (cache.empty())
            {
                cache = datadir::Toolchains(); // <data root>/toolchains (datadir.h)
                if (cache.empty())
                    cache = build.work / "toolchains";
            }
            root = DownloadToolchain(build, cache);
        }
        build.vars["CXX"] = Slashes(root / Compiler.compiler);
        Log("compiler: " + build.vars["CXX"]);
#else
        if (build.cross)
        {
            fs::path clang = build.options.toolchain / "bin" / "x86_64-w64-mingw32-clang++";
            if (!fs::exists(clang))
                Fail("a Windows kit on this OS needs --toolchain <an llvm-mingw 20260922 release for this OS>");
            build.vars["CXX"] = clang.string();
            Log("compiler: " + clang.string() + " (cross build)");
            return;
        }
#ifdef __linux__
        if (build.kit["zig"] != ZigVersion)
            Fail("the kit was built with zig " + build.kit["zig"] + ", this SSABuild uses " + ZigVersion);
        fs::path cache = build.options.toolchain;
        fs::path root;
        if (!cache.empty() && fs::exists(cache / Compiler.compiler))
            root = cache;
        else
        {
            if (cache.empty())
            {
                cache = datadir::Toolchains(); // <data root>/toolchains (datadir.h), as on Windows
                if (cache.empty())
                    cache = build.work / "toolchains";
            }
            root = DownloadToolchain(build, cache);
        }
        // zig builds its libc++ for this target on the first link (a few minutes, once) into its global cache, next
        // to the compiler; its local cache keeps a copy of every object and of the game, so it goes in the work
        // folder and is emptied after each compile and link (ClearCompilerCache).
        SetEnv("ZIG_GLOBAL_CACHE_DIR", (root.parent_path() / (std::string(Compiler.folder) + "-cache")).string());
        SetEnv("ZIG_LOCAL_CACHE_DIR", (build.work / "zig-cache").string());
        build.vars["CXX"] = (root / Compiler.compiler).string();
        Log("compiler: " + build.vars["CXX"] + " (" + Compiler.name + ")");
#else
        fs::path out = build.work / "logs" / "xcrun.txt";
        RunOptions run;
        run.log = out;
        if (Run({ "xcrun", "--find", "clang++" }, run) != 0)
            Fail("Xcode's command line tools are missing: run xcode-select --install, then try again");
        std::string clang = ReadText(out);
        clang.erase(clang.find_last_not_of("\r\n ") + 1);
        if (Run({ "xcrun", "--show-sdk-path" }, run) != 0)
            Fail("no macOS SDK found (xcrun --show-sdk-path)");
        std::string sdk = ReadText(out);
        sdk.erase(sdk.find_last_not_of("\r\n ") + 1);
        // The /usr/bin driver passes the SDK on; the toolchain's own clang++ would not find the headers.
        build.vars["CXX"] = "/usr/bin/clang++";
        build.vars["SDK"] = sdk;
        Log("compiler: " + clang + ", SDK " + sdk);
        if (Run({ "xcrun", "-sdk", "macosx", "metal", "--version" }, run) != 0)
            Fail("the Metal shader compiler is missing: run xcodebuild -downloadComponent MetalToolchain (Xcode 26+)");
#endif
#endif
    }

    // --- disc ------------------------------------------------------------------------------------------------------

    // Identifies the disc image cheaply (path, size, time): a rerun with the same image skips the disc steps.
    std::string DiscStamp(const Build& build)
    {
        std::error_code error;
        // A copy of the game data (a folder, game_data.h) is stamped by its manifest, which changes with the copy.
        fs::path stamped = fs::is_directory(build.options.iso, error) ? build.options.iso / "gamedata.manifest" : build.options.iso;
        auto size = fs::file_size(stamped, error);
        auto time = std::chrono::duration_cast<std::chrono::seconds>(
            fs::last_write_time(stamped, error).time_since_epoch()).count();
        return "game=" + build.game.id + "\niso=" + Slashes(fs::absolute(build.options.iso)) + "\nsize=" + std::to_string(size) + "\ntime=" +
            std::to_string(static_cast<long long>(time)) + "\n";
    }

    // The kit's games and which one the disc is: default.xex's SHA-256 against every config/<id>/game.txt. An unknown
    // disc stops here, before anything is downloaded or built.
    void IdentifyGame(Build& build)
    {
        std::vector<Game> games;
        for (const std::string& id : SplitList(build.kit["games"]))
        {
            auto values = ReadKeyValues(build.options.kit / "config" / id / "game.txt");
            if (values.empty())
                Fail("the kit lists the game " + id + " but has no config/" + id + "/game.txt");
            games.push_back({ id, values["name"], values["runtime"], values["toml"], values["shaders"],
                SplitList(values["xex_sha256"]), values["movie_portal"], values["movie_press"],
                unsigned(strtoul(values["movie_shaders"].c_str(), nullptr, 10)) });
        }
        if (games.empty())
            Fail("this kit lists no game (kit.txt has no games=)");

        DiscFileSystem fileSystem;
        if (!fileSystem.Open(Slashes(build.options.iso).c_str()))
            Fail("not an Xbox 360 disc image: " + Slashes(build.options.iso));
        std::vector<uint8_t> xex = fileSystem.ReadAll("default.xex");
        if (xex.empty())
            Fail("the disc has no default.xex: it is not an Xbox 360 game disc");
        std::string sha = Sha256Text(std::string(reinterpret_cast<const char*>(xex.data()), xex.size()));
        for (const Game& game : games)
            if (std::find(game.xexSha256.begin(), game.xexSha256.end(), sha) != game.xexSha256.end())
            {
                build.game = game;
                Log("game: " + game.name + " (default.xex " + sha.substr(0, 12) + "...)");
                return;
            }
        std::string supported;
        for (const Game& game : games)
            supported += (supported.empty() ? "" : ", ") + game.name;
        Fail("this disc is not one of the games these tools build (" + supported + "): update the game tools (default.xex SHA-256 " + sha + ")");
    }

    void ReadDisc(Build& build)
    {
        Step("disc", "Reading the disc");
        fs::path disc = build.work / "disc";
        fs::path input = build.work / "shaders" / "input";
        fs::path stamp = disc / "stamp.txt";
        std::string expected = DiscStamp(build);
        bool fromArchive = build.game.shaders.rfind("igz:", 0) == 0;
        // What the shader step needs is on disk: the XEX image (and the archives) for "lsa", the containers for "igz".
        auto haveShaders = [&]()
        {
            if (!fromArchive)
                return fs::exists(input / "default.xex.image");
            std::error_code error;
            for (const auto& entry : fs::directory_iterator(input, error))
                if (entry.path().extension() == ".vs" || entry.path().extension() == ".ps")
                    return true;
            return false;
        };
        if (ReadText(stamp) == expected && fs::exists(disc / "default.xex") && haveShaders())
        {
            Log("up to date");
            return;
        }
        fs::remove(stamp);
        fs::create_directories(disc);
        fs::create_directories(input);

        DiscFileSystem fileSystem;
        if (!fileSystem.Open(Slashes(build.options.iso).c_str()))
            Fail("not an Xbox 360 disc image: " + Slashes(build.options.iso));
        std::vector<uint8_t> xex = fileSystem.ReadAll("default.xex");
        if (xex.empty())
            Fail("the disc has no default.xex: is it " + build.game.name + "?");
        WriteBytes(disc / "default.xex", xex.data(), xex.size());

        if (fromArchive)
        {
            // Alchemy: every shader is in one IGA archive (games/giants/docs/shaders.md, games/swapforce/docs/shaders.md).
            std::string path = build.game.shaders.substr(4);
            std::vector<uint8_t> archive = fileSystem.ReadAll(path);
            Progress(1, 3);
            ShaderExtraction extraction;
            if (archive.empty() || !ExtractArchiveShaders(archive, input, extraction))
                Fail("the disc has no readable " + path + ": is it " + build.game.name + "?");
            Log(path + ": " + std::to_string(extraction.entries) + " entries, " + std::to_string(extraction.igz) + " IGZ, " +
                std::to_string(extraction.vertex + extraction.pixel) + " shaders (" + std::to_string(extraction.vertex) +
                " vertex, " + std::to_string(extraction.pixel) + " pixel)");
            if (extraction.vertex + extraction.pixel == 0)
                Fail("no shader found in " + path + ": is this the right disc?");
            Progress(3, 3);
        }
        else
        {
            for (const char* name : { "shaders.lsa", "effects.lsa" })
            {
                std::vector<uint8_t> data = fileSystem.ReadAll(name);
                if (data.empty())
                    Fail(std::string("the disc has no ") + name + ": is it " + build.game.name + "?");
                WriteBytes(input / name, data.data(), data.size());
            }
            Progress(3, 4);
            // The loaded image of default.xex: XenosRecomp finds the shaders embedded in it.
            Image image = Image::ParseImage(xex.data(), xex.size());
            if (image.data == nullptr)
                Fail("cannot decrypt default.xex");
            WriteBytes(input / "default.xex.image", image.data.get(), image.size);
            Log("default.xex: " + std::to_string(xex.size() >> 10) + " KB, image " + std::to_string(image.size >> 10) + " KB");
            Progress(4, 4);
        }
        WriteText(stamp, expected);
    }

    // --- game code -------------------------------------------------------------------------------------------------

    void TranslateCode(Build& build)
    {
        Step("recompile", "Translating the game code");
        fs::path ppc = build.work / "ppc";
        fs::path stamp = ppc / "stamp.txt";
        // What the translation depends on: the disc, XenonRecomp itself, its config and the context header it is given,
        // and the headers the kit's runtime expects. Not the kit's build or folder: a runtime-only update keeps ppc/.
        std::map<std::string, std::string> files;
        for (const auto& entry : fs::directory_iterator(GameDir(build, "config")))
            if (entry.path().filename() != "game.txt")
                files[Slashes(entry.path().filename())] = Sha256File(entry.path());
        std::string config;
        for (const auto& [name, sha] : files)
            config += name + "=" + sha + ",";
        std::string expected = DiscStamp(build) + "config=" + Sha256Text(config) + "\ntranslator=" +
            Sha256File(build.options.kit / "bin" / (std::string("XenonRecomp") + Exe)) + "\ncontext=" +
            Sha256File(build.options.kit / "share" / "ppc_context.h") + "\nkit_config=" +
            build.kit["ppc_config_sha256." + build.game.id] + "\nkit_context=" + build.kit["ppc_context_sha256"] + "\n";
        if (ReadText(stamp) == expected)
        {
            Log("up to date");
            return;
        }

        if (build.cross)
            Fail("a cross build reuses ppc/ and the shader cache of a native run (docs/tools/ssabuild.md)");
        // XenonRecomp reads its paths relative to the TOML (../private/default.xex, ../ppc): lay it out that way, with
        // relative paths only (its file APIs may not take UTF-8).
        fs::path xenon = build.work / "xenon";
        fs::remove_all(xenon);
        fs::create_directories(xenon / "config");
        fs::create_directories(xenon / "private");
        fs::create_directories(xenon / "ppc");
        fs::create_directories(ppc);
        for (const auto& entry : fs::directory_iterator(GameDir(build, "config")))
            if (entry.path().filename() != "game.txt")
                fs::copy_file(entry.path(), xenon / "config" / entry.path().filename());
        fs::copy_file(build.work / "disc" / "default.xex", xenon / "private" / "default.xex");
        fs::copy_file(build.options.kit / "share" / "ppc_context.h", xenon / "ppc_context.h");

        RunOptions run;
        run.cwd = xenon;
        run.log = build.work / "logs" / "xenonrecomp.log";
        int result = Run({ Slashes(build.options.kit / "bin" / (std::string("XenonRecomp") + Exe)), "config/" + build.game.toml,
            "ppc_context.h" }, run);
        if (result != 0 || !fs::exists(xenon / "ppc" / "ppc_config.h"))
        {
            LogTail(run.log);
            Fail("XenonRecomp failed (exit code " + std::to_string(result) + ")");
        }

        // The kit's runtime was compiled against these two headers: another disc version would not match it.
        for (const char* header : { "ppc_config.h", "ppc_context.h" })
        {
            std::string key = std::string(header) == "ppc_config.h" ? "ppc_config_sha256." + build.game.id : "ppc_context_sha256";
            if (Sha256File(xenon / "ppc" / header) != build.kit[key])
                Fail(std::string("this disc does not match the kit (") + header + " differs): the kit's " + build.game.name +
                    " runtime was built for another version of the game");
        }

        size_t changed = 0;
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(xenon / "ppc"))
        {
            count++;
            if (CopyIfDifferent(entry.path(), ppc / entry.path().filename()))
                changed++;
        }
        fs::remove_all(xenon);
        Log(std::to_string(count) + " files, " + std::to_string(changed) + " changed");
        WriteText(stamp, expected);
    }

    // --- shaders ---------------------------------------------------------------------------------------------------

    std::string RuntimeShaderList(const Build& build)
    {
        std::set<std::string> names;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(build.work / "shaders" / "input" / "runtime", error))
            names.insert(Slashes(entry.path().filename()));
        std::string list;
        for (const auto& name : names)
            list += name + ",";
        return list;
    }

    void TranslateShaders(Build& build)
    {
        Step("shaders", "Translating the shaders");
        fs::path shaders = build.work / "shaders";
        fs::path stamp = shaders / "stamp.txt";
        fs::path cache = build.work / "ppc" / "shader_cache.cpp";
        // The translator and shader_common.h decide the translated text; the compiled results are kept by the text's
        // hash (below), so a new translator redoes the translation but only compiles what changed.
        fs::path xenos = build.options.kit / "bin" / (std::string("XenosRecomp") + Exe);
        std::string expected = DiscStamp(build) + "runtime=" + RuntimeShaderList(build) + "\ntranslator=" + Sha256File(xenos) +
            "\ncommon=" + Sha256File(build.options.kit / "share" / "shader_common.h") + "\n";
        if (ReadText(stamp) == expected && fs::exists(cache))
        {
            Log("up to date");
            return;
        }
        if (build.cross)
            Fail("a cross build reuses ppc/ and the shader cache of a native run (docs/tools/ssabuild.md)");
        fs::remove(stamp);
        fs::copy_file(build.options.kit / "share" / "shader_common.h", shaders / "shader_common.h",
            fs::copy_options::overwrite_existing);

        // The compiled shaders stay in shaders/cache, keyed by the hash of their translated text (XenosRecomp): the movie
        // pass and a game update with a new translator compile only the shaders whose text is new. They are valid for
        // the same compilers only: the kit's DXC libraries (XenosRecomp checks shader_common.h itself).
        fs::path cacheStamp = shaders / "cache.txt";
        std::map<std::string, std::string> tools;
        for (const auto& entry : fs::directory_iterator(build.options.kit / "bin"))
        {
            std::string file = Slashes(entry.path().filename());
            if (file.find("XenonRecomp") == std::string::npos && file.find("XenosRecomp") == std::string::npos)
                tools[file] = Sha256File(entry.path());
        }
        std::string compilers;
        for (const auto& [name, sha] : tools)
            compilers += name + "=" + sha + ",";
        std::string cacheId = "compilers=" + Sha256Text(compilers) + "\n";
        if (ReadText(cacheStamp) != cacheId)
        {
            std::error_code error;
            fs::remove_all(shaders / "cache", error);
            WriteText(cacheStamp, cacheId);
        }

        // XenosRecomp prints "Progress <done> <total>" (flushed) as it goes: the step's progress bar.
        std::pair<uint64_t, uint64_t> shown{ 0, 0 };
        RunOptions run;
        run.cwd = build.work;
        run.log = build.work / "logs" / "xenosrecomp.log";
        run.timeoutSeconds = 30 * 60;
        run.poll = [&]()
        {
            std::ifstream log(run.log, std::ios::binary | std::ios::ate);
            if (!log)
                return false;
            std::streamoff size = log.tellg();
            std::string tail(size_t(std::min<std::streamoff>(size, 4096)), '\0');
            log.seekg(size - std::streamoff(tail.size()));
            log.read(tail.data(), std::streamsize(tail.size()));
            size_t at = tail.rfind("Progress ");
            unsigned long long done = 0, total = 0;
            if (at != std::string::npos && sscanf(tail.c_str() + at, "Progress %llu %llu", &done, &total) == 2 && total > 0 &&
                (done != shown.first || total != shown.second))
            {
                shown = { done, total };
                Progress(done, total);
            }
            return false;
        };
        int result = Run({ Slashes(xenos), "shaders/input", "shaders/shader_cache.cpp", "shaders/shader_common.h",
            "shaders/cache" }, run);
        if (result != 0 || !fs::exists(shaders / "shader_cache.cpp"))
        {
            LogTail(run.log);
            Fail("XenosRecomp failed (exit code " + std::to_string(result) + ")");
        }
        // The shaders XenosRecomp could not translate (the movie step must not feed them back).
        std::string skipped;
        std::ifstream log(run.log);
        for (std::string line; std::getline(log, line);)
        {
            if (line.find("shaders translated") != std::string::npos || line.find("without DXIL") != std::string::npos)
                Log(line);
            if (line.rfind("Skipped ", 0) == 0)
                skipped += line.substr(8, 16) + "\n";
        }
        WriteText(shaders / "skipped.txt", skipped);
        CopyIfDifferent(shaders / "shader_cache.cpp", cache);
        fs::remove(shaders / "shader_cache.cpp");
        WriteText(stamp, expected);
    }

    // --- compile and link ------------------------------------------------------------------------------------------

    // zig's copies of what it just built (Linux): SSABuild keeps its own objects in obj/.
    void ClearCompilerCache(const Build& build)
    {
        std::error_code error;
        fs::remove_all(build.work / "zig-cache", error);
    }

    std::vector<std::string> ObjectList(const Build& build)
    {
        std::vector<std::string> objects;
        for (const auto& entry : fs::directory_iterator(build.work / "ppc"))
            if (entry.path().extension() == ".cpp")
                objects.push_back("obj/" + Slashes(entry.path().stem()) + ".o");
        std::sort(objects.begin(), objects.end());
        return objects;
    }

    // Compiles every ppc/*.cpp whose object is missing, older than it, or made by another command or with other headers.
    // "Other" is decided by content: the command line with the kit's folder written as <kit>, and a digest of the
    // ppc/ headers and of the kit's include/ (an update unpacks the kit in a new folder with new file times, and its
    // headers are mostly the same).
    void Compile(Build& build)
    {
        Step("compile", "Compiling the game");
        fs::path ppc = build.work / "ppc";
        fs::path obj = build.work / "obj";
        fs::create_directories(obj);

        std::map<std::string, std::string> headerFiles;
        std::vector<fs::path> sources;
        for (const auto& entry : fs::directory_iterator(ppc))
        {
            if (entry.path().extension() == ".h")
                headerFiles[Slashes(entry.path().filename())] = Sha256File(entry.path());
            else if (entry.path().extension() == ".cpp")
                sources.push_back(entry.path());
        }
        std::string headers = "include=" + ContentDigest(build.options.kit / "include") + "\n";
        for (const auto& [name, sha] : headerFiles)
            headers += name + "=" + sha + "\n";

        struct Job
        {
            fs::path source;
            std::vector<std::string> args;
            uintmax_t size;
            fs::path keyFile;
            std::string key;
        };
        std::vector<Job> jobs;
        for (const fs::path& source : sources)
        {
            std::string name = Slashes(source.stem());
            build.vars["SRC"] = "ppc/" + Slashes(source.filename());
            build.vars["OBJ"] = "obj/" + name + ".o";
            bool cache = name == "shader_cache";
            std::vector<std::string> args = ExpandTemplate(build, cache ? "shader_cache.args" : "compile.args", {});
            fs::path rsp = obj / (name + ".rsp");
            fs::path keyFile = obj / (name + ".key");
            fs::path object = obj / (name + ".o");
            std::string key = Sha256Text(CommandText(build, args) + headers);
            args = WithResponseFile(args, rsp);
            bool fresh = ReadText(keyFile) == key && fs::exists(object) && TimeOf(object) >= TimeOf(source);
            if (!fresh)
            {
                fs::remove(object);
                fs::remove(keyFile);
                jobs.push_back({ source, args, fs::file_size(source), keyFile, key });
            }
        }
        build.vars.erase("SRC");
        build.vars.erase("OBJ");

        size_t total = sources.size();
        size_t done = total - jobs.size();
        if (jobs.empty())
        {
            Log("up to date");
            return;
        }
        // Largest first (the shader cache and the biggest ppc files are the long poles).
        std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.size > b.size; });
        Log("compiling " + std::to_string(jobs.size()) + " of " + std::to_string(total) + " files with " +
            std::to_string(build.options.jobs) + " jobs");
        Progress(done, total);

        std::atomic<size_t> next = 0;
        std::atomic<size_t> finished = done;
        std::atomic<bool> failed = false;
        std::string failure;
        std::mutex failureMutex;
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < std::min<size_t>(build.options.jobs, jobs.size()); t++)
        {
            threads.emplace_back([&]()
            {
                while (!failed)
                {
                    size_t index = next++;
                    if (index >= jobs.size())
                        return;
                    const Job& job = jobs[index];
                    RunOptions run;
                    run.cwd = build.work;
                    run.log = obj / (Slashes(job.source.stem()) + ".log");
                    int result = Run(job.args, run);
                    if (result != 0)
                    {
                        std::lock_guard lock(failureMutex);
                        if (!failed.exchange(true))
                        {
                            LogTail(run.log, 30);
                            failure = "compiling " + Slashes(job.source.filename()) + " failed (exit code " +
                                std::to_string(result) + ")";
                        }
                        return;
                    }
                    std::error_code error;
                    fs::remove(run.log, error);
                    WriteText(job.keyFile, job.key);
                    Progress(++finished, total);
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        ClearCompilerCache(build);
        if (failed)
            Fail(failure);
    }

    void CopyTree(const fs::path& from, const fs::path& to)
    {
        fs::create_directories(to);
        for (const auto& entry : fs::recursive_directory_iterator(from))
        {
            fs::path target = to / fs::relative(entry.path(), from);
            if (entry.is_directory())
                fs::create_directories(target);
            else
                CopyIfDifferent(entry.path(), target);
        }
    }

    // macOS: the game's own icon, made from the dashboard icon of the player's default.xex (a PNG in its XDBF resource), as
    // the bundle's .icns (sips scales it, iconutil packs it). Returns the kit's neutral icon when the disc has no icon or
    // a tool fails: the bundle always has one.
    fs::path BundleIcon(const Build& build)
    {
        fs::path neutral = build.options.kit / "res" / "SSARecomp.icns";
        fs::path dir = build.work / "icon";
        fs::path icns = dir / "SSARecomp.icns";
        fs::path xex = build.work / "disc" / "default.xex";
        std::error_code error;
        if (fs::exists(icns) && fs::last_write_time(icns, error) >= fs::last_write_time(xex, error))
            return icns;
        std::string bytes = ReadText(xex);
        TitleMetadata title = ReadTitleMetadata(std::vector<uint8_t>(bytes.begin(), bytes.end()));
        if (title.iconPng.empty())
        {
            Log("icon: default.xex has no title icon, using the kit's");
            return neutral;
        }
        fs::path set = dir / "SSARecomp.iconset";
        fs::remove_all(set, error);
        fs::create_directories(set);
        fs::path png = dir / "title_icon.png";
        WriteBytes(png, title.iconPng.data(), title.iconPng.size());
        RunOptions run;
        run.log = build.work / "logs" / "icon.log";
        bool ok = true;
        // The sizes an iconset holds: 16, 32, 128, 256 and 512 points, each at 1x and 2x.
        for (int points : { 16, 32, 128, 256, 512 })
        {
            for (int scale : { 1, 2 })
            {
                std::string pixels = std::to_string(points * scale);
                std::string name = "icon_" + std::to_string(points) + "x" + std::to_string(points) + (scale == 2 ? "@2x" : "") + ".png";
                ok = ok && Run({ "sips", "-z", pixels, pixels, Slashes(png), "--out", Slashes(set / name) }, run) == 0;
            }
        }
        ok = ok && Run({ "iconutil", "-c", "icns", "-o", Slashes(icns), Slashes(set) }, run) == 0;
        if (!ok || !fs::exists(icns))
        {
            LogTail(run.log);
            Log("icon: could not make the game's icon, using the kit's");
            return neutral;
        }
        Log("icon: the game's own, from default.xex");
        return icns;
    }

    void Link(Build& build)
    {
        Step("link", "Linking the game");
        fs::path res = build.options.kit / "res";
        const std::string& name = build.game.runtime; // SSARecomp, SGRecomp
        fs::path app = build.options.out / (name + ".app");
        fs::create_directories(build.options.out);
        if (build.macos)
        {
            fs::path contents = app / "Contents";
            fs::create_directories(contents / "MacOS");
            fs::create_directories(contents / "Resources");
            CopyIfDifferent(res / build.game.id / "Info.plist", contents / "Info.plist");
            CopyIfDifferent(BundleIcon(build), contents / "Resources" / "SSARecomp.icns"); // the plist names it
            CopyIfDifferent(res / "THIRD_PARTY_NOTICES.md", contents / "Resources" / "THIRD_PARTY_NOTICES.md");
            CopyTree(res / "fonts", contents / "Resources" / "fonts");
            build.exe = contents / "MacOS" / name;
        }
        else
        {
            CopyIfDifferent(res / "THIRD_PARTY_NOTICES.md", build.options.out / "THIRD_PARTY_NOTICES.md");
            CopyTree(res / "fonts", build.options.out / "fonts");
            build.exe = build.options.out / (build.platform == "windows-x64" ? name + ".exe" : name);
        }
        build.vars["OUT"] = Slashes(build.exe);
        std::vector<std::string> objects = ObjectList(build);
        // The link is fresh when its command (the kit's folder aside) and the content of the kit's libraries are the same.
        std::vector<std::string> command = ExpandTemplate(build, "link.args", objects);
        std::string key = CommandText(build, command);
        const std::string kitPrefix = Slashes(build.options.kit);
        for (const std::string& arg : command)
        {
            std::error_code error;
            if (arg.rfind(kitPrefix, 0) == 0 && fs::is_regular_file(PathFromUtf8(arg.c_str()), error))
                key += Sha256File(PathFromUtf8(arg.c_str())) + "\n";
        }
        key = Sha256Text(key);
        fs::path linkKey = build.work / "link.key";
        std::vector<std::string> args = WithResponseFile(command, build.work / "link.rsp");
        bool fresh = fs::exists(build.exe) && ReadText(linkKey) == key;
        for (const std::string& object : objects)
            fresh = fresh && TimeOf(build.work / object) <= TimeOf(build.exe);
        if (fresh)
        {
            Log("up to date");
            return;
        }

        fs::remove(linkKey);
        RunOptions run;
        run.cwd = build.work;
        run.log = build.work / "logs" / "link.log";
        int result = Run(args, run);
        ClearCompilerCache(build);
        if (result != 0)
        {
            LogTail(run.log, 30);
            Fail("linking failed (exit code " + std::to_string(result) + "); if the game is running, close it");
        }
        if (build.macos)
        {
            RunOptions sign;
            sign.log = build.work / "logs" / "codesign.log";
            if (Run({ "codesign", "--force", "--sign", "-", "--timestamp=none", app.string() }, sign) != 0)
            {
                LogTail(sign.log);
                Fail("codesign failed");
            }
        }
        WriteText(linkKey, key);
        Log(Slashes(build.exe) + ": " + std::to_string(fs::file_size(build.exe) >> 20) + " MB");
    }

    // --- movie shaders ---------------------------------------------------------------------------------------------

    // Hashes of the shader cache's entries ("\t{ 0x<hash>, ...").
    std::set<uint64_t> CachedShaders(const Build& build)
    {
        std::set<uint64_t> hashes;
        std::ifstream file(build.work / "ppc" / "shader_cache.cpp");
        for (std::string line; std::getline(file, line);)
        {
            if (line.rfind("\t{ 0x", 0) == 0)
                hashes.insert(strtoull(line.c_str() + 5, nullptr, 16));
            else if (!hashes.empty())
                break;
        }
        return hashes;
    }

    // The game builds the Bink movie shaders at run time (they are in no archive): run it headless for a few seconds
    // with SSA_DUMP_SHADERS, keep the containers the cache lacks, then rebuild the cache with them.
    bool AddMovieShaders(Build& build)
    {
        Step("movies", "Adding the movie shaders");
        fs::path movies = build.work / "movies";
        fs::path stamp = movies / "done.txt";
        if (fs::exists(stamp))
        {
            Log("up to date");
            return false;
        }
        if (build.cross)
        {
            Log("skipped: the game cannot run on this OS");
            return false;
        }
        fs::remove_all(movies);
        fs::create_directories(movies / "dump");
        fs::create_directories(movies / "saves");
        fs::create_directories(movies / "collection");

        // Known: in the cache, or in the disc's shaders but untranslatable (feeding those again gains nothing).
        std::set<uint64_t> cached = CachedShaders(build);
        std::ifstream skipped(build.work / "shaders" / "skipped.txt");
        for (std::string line; std::getline(skipped, line);)
            cached.insert(strtoull(line.c_str(), nullptr, 16));
        auto missing = [&]()
        {
            std::vector<fs::path> files;
            std::error_code error;
            for (const auto& entry : fs::directory_iterator(movies / "dump", error))
                if (cached.count(strtoull(Slashes(entry.path().stem()).c_str(), nullptr, 16)) == 0)
                    files.push_back(entry.path());
            return files;
        };

        // Nothing of the player's: no settings file, scratch data folder, log, saves and collection, no portal, no sound,
        // no window (SKYLANDERS_DATA moves every folder the game writes, SSA_LOG its log). Test
        // variables a launcher may pass on (screenshots, skipped movies, presses) are removed ("": unset). A game whose
        // run-time shaders come later (game.txt: Swap Force's loading screen, after the menus) gets an empty emulated
        // portal (without one it stops at "PORTAL DISCONNECTED") and presses.
        const Game& game = build.game;
        const std::string portal = game.moviePortal.empty() ? "none" : game.moviePortal;
        const std::pair<const char*, const char*> environment[] = {
            { "SSA_HEADLESS", "1" }, { "SSA_DUMP_SHADERS", "dump" }, { "SSA_SETTINGS", "settings.ini" },
            { "SSA_SAVE_DIR", "saves" }, { "SSA_COLLECTION", "collection" }, { "SSA_PORTAL", portal.c_str() },
            { "SSA_PORTAL_PLACE", "" }, { "SDL_AUDIO_DRIVER", "dummy" }, { "SSA_LAUNCHER_SHOTS", "" },
            { "SSA_SKIP_MOVIES", "" }, { "SSA_PRESS", game.moviePress.c_str() },
            { "SKYLANDERS_DATA", "data" }, { "SSA_LAUNCHER_INI", "none.ini" }, { "SSA_LOG", "runtime.log" } };
        std::vector<std::string> previous; // the caller's values, put back after the run
        for (const auto& [name, value] : environment)
        {
            previous.push_back(GetEnv(name));
            SetEnv(name, value);
        }

        auto start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point firstNew{};
        RunOptions run;
        run.cwd = movies;
        run.log = movies / "game.log";
        run.timeoutSeconds = 120;
        run.poll = [&]()
        {
            auto now = std::chrono::steady_clock::now();
            Progress(uint64_t(std::chrono::duration_cast<std::chrono::seconds>(now - start).count()), 120);
            size_t found = missing().size();
            if (game.movieShaders != 0)
                return found >= game.movieShaders;
            if (firstNew == std::chrono::steady_clock::time_point{} && found != 0)
                firstNew = now;
            // The movie shaders come together when the first movie starts: give the rest a few seconds.
            return firstNew != std::chrono::steady_clock::time_point{} && now - firstNew > std::chrono::seconds(8);
        };
        Log("running the game headless to collect the shaders it builds at run time");
        int result = Run({ ShortPath(build.exe), ShortPath(build.options.iso) }, run);
        for (size_t i = 0; i < std::size(environment); i++)
            SetEnv(environment[i].first, previous[i]);

        std::vector<fs::path> found = missing();
        if (found.empty())
        {
            LogTail(run.log, 15);
            Log("warning: the run (exit code " + std::to_string(result) +
                ") produced no new shader; movies may not be drawn");
            return false;
        }
        fs::path runtime = build.work / "shaders" / "input" / "runtime";
        fs::create_directories(runtime);
        for (const fs::path& file : found)
            CopyIfDifferent(file, runtime / file.filename());
        Log(std::to_string(found.size()) + " shaders built at run time (Bink movies)");
        if (game.movieShaders != 0 && found.size() < game.movieShaders)
            Log("warning: " + std::to_string(game.movieShaders) + " expected; the missing ones' draws are skipped");
        WriteText(stamp, std::to_string(found.size()) + "\n");
        return true;
    }

    void Usage()
    {
        fprintf(stderr, "usage: SSABuild --iso <disc image> --out <game dir> [--kit <kit dir>] [--jobs N] "
            "[--toolchain <dir>]\n");
    }
}

int main(int argc, char** argv)
{
    Build build;
    Options& options = build.options;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        bool hasValue = i + 1 < argc;
        if (arg == "--iso" && hasValue)
            options.iso = PathFromUtf8(argv[++i]);
        else if (arg == "--out" && hasValue)
            options.out = PathFromUtf8(argv[++i]);
        else if (arg == "--kit" && hasValue)
            options.kit = PathFromUtf8(argv[++i]);
        else if (arg == "--toolchain" && hasValue)
            options.toolchain = PathFromUtf8(argv[++i]);
        else if (arg == "--jobs" && hasValue)
            options.jobs = unsigned(std::max(1, atoi(argv[++i])));
        else
        {
            Usage();
            return 2;
        }
    }
    if (options.iso.empty() || options.out.empty())
    {
        Usage();
        return 2;
    }

    auto started = std::chrono::steady_clock::now();
    try
    {
        if (options.kit.empty())
            options.kit = ExecutableDirectory();
        options.iso = fs::absolute(options.iso);
        options.out = fs::absolute(options.out);
        options.kit = fs::absolute(options.kit);
        if (!options.toolchain.empty())
            options.toolchain = fs::absolute(options.toolchain);
        if (!fs::is_regular_file(options.iso) && !fs::is_regular_file(options.iso / "gamedata.manifest"))
            Fail("no disc image at " + Slashes(options.iso));
        build.kit = ReadKeyValues(options.kit / "kit.txt");
        if (build.kit.empty())
            Fail("no kit at " + Slashes(options.kit) + " (kit.txt is missing)");
        build.platform = build.kit["platform"];
        build.macos = build.platform.rfind("macos", 0) == 0;
#if defined(_WIN32)
        bool native = build.platform == "windows-x64";
#elif defined(__APPLE__)
        bool native = build.macos;
#elif defined(__linux__) && defined(__x86_64__)
        bool native = build.platform == "linux-x64";
#else
        bool native = false;
#endif
        build.cross = !native && build.platform == "windows-x64";
        if (!native && !build.cross)
            Fail("this kit is for " + build.platform);

        // About 2 GB of memory per compiler on the largest files.
        if (options.jobs == 0)
        {
            unsigned threads = std::max(1u, std::thread::hardware_concurrency());
            unsigned memory = unsigned(PhysicalMemoryBytes() / (2ull << 30));
            options.jobs = std::max(1u, std::min(threads, memory));
        }

        build.work = options.out / "ssabuild-work";
        fs::create_directories(build.work / "logs");
        build.vars["KIT"] = Slashes(options.kit);
        build.vars["PPC"] = "ppc";
        Log("SSABuild " + build.kit["version"] + " (" + build.kit["build"] + ", " + build.platform + "), kit " +
            Slashes(options.kit));

        std::error_code error;
        auto space = fs::space(options.out, error);
        if (!error && space.available < (6ull << 30))
            Log("warning: only " + std::to_string(space.available >> 20) + " MB free on the game drive; about 5 GB "
                "are needed");

        IdentifyGame(build);
        build.vars["GAME"] = build.game.id;
        PrepareToolchain(build);
        ReadDisc(build);
        TranslateCode(build);
        TranslateShaders(build);
        Compile(build);
        Link(build);
        if (AddMovieShaders(build))
        {
            // The game built above runs, without the movies: keep it if this second pass fails (a rerun retries it).
            try
            {
                TranslateShaders(build);
                Compile(build);
                Link(build);
            }
            catch (const Failure& failure)
            {
                Log("warning: " + failure.message + "; the game is built but movies may not be drawn");
            }
        }
        EndStep();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
        Log("finished in " + std::to_string(seconds.count()) + " s");
        Emit("done", Slashes(build.exe));
        return 0;
    }
    catch (const Failure& failure)
    {
        EndStep();
        Emit("error", failure.message);
    }
    catch (const std::exception& exception)
    {
        Emit("error", exception.what());
    }
    return 1;
}
