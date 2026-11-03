// Arkeyan-Builder: builds the game on the player's machine from their disc and the player kit (docs/tools/arkeyan-builder.md).
//
//   Arkeyan-Builder --iso <iso or copied game data folder> --out <game dir> [--kit <kit dir>] [--jobs N] [--toolchain <dir>]
//
// The kit holds several games (kit.txt: games=ssa,giants,swapforce,trapteam). The disc's default.xex, by its SHA-256,
// identifies one (config/<id>/game.txt); everything below then comes from that game's part of the kit.
//
// Steps: the compiler (llvm-mingw on Windows, zig's clang on Linux: downloaded and checked; Xcode's clang on macOS),
// the disc's files
// (default.xex, its loaded image, the shader archives), the game code (XenonRecomp, a separate program of the kit), the
// shaders (XenosRecomp, likewise), compiling and linking against the kit's prebuilt runtime, then a short headless run
// of the new game that saves the shaders it builds at run time (Bink movies) and a rebuild of the shader cache with
// them. Work files stay in <game dir>/arkeyan-work (or its existing ssabuild-work), so a rerun only redoes what changed.
#include "host/collection/datadir.h"
#include "host/env.h"
#include <stb_image.h>
#include "host/xex_title.h"
#include "game_icon.h"
#include "game_config.h"
#include "install_guard.h"
#include "host/art/game_art.h"
#include "igz_shaders.h"
#include "menu_music.h"
#include "apu/xma_decoder.h"
#include "process.h"
#include "shader_cache_literals.h"
#include "sha256.h"
#include "version_resource.h"
#include "vfs.h"
#include <image.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
    using namespace skybuild;
    // The compiler Arkeyan-Builder downloads: the one the kit's runtime was built with (tools/make_kit.sh), pinned by SHA-256.
    struct Toolchain
    {
        const char* name;
        const char* folder; // the archive's top folder
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
    // the game targets glibc 2.34 whatever the distribution (docs/tools/arkeyan-builder.md). The kit records the same version.
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
            auto seconds =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_stepStart);
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
    void Progress(uint64_t done, uint64_t total)
    {
        Emit("progress", std::to_string(done) + " " + std::to_string(total));
    }

    struct Failure
    {
        std::string message;
    };

    [[noreturn]] void Fail(const std::string& message) { throw Failure{ message }; }

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
        file.close();
        if (!file)
            Fail("cannot write " + Slashes(path));
    }

    void WriteBytes(const fs::path& path, const void* data, size_t size)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), std::streamsize(size));
        file.close();
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
        if (fs::is_regular_file(path))
            return Sha256File(path);
        std::map<std::string, std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(path))
            if (entry.is_regular_file())
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

    struct Options
    {
        fs::path iso;
        fs::path out;
        fs::path kit;
        fs::path toolchain;
        unsigned jobs = 0;
        bool iconOnly = false; // --icon-only: make the game's icon (GameIcon) and stop; a check, no build
        fs::path
            menuMusic; // --menu-music <file.wav>: write the game's main menu theme there (MenuMusic) and stop; no build
    };

    struct Build
    {
        Options options;
        Game game;
        fs::path work;
        std::map<std::string, std::string> kit; // kit.txt
        std::string platform; // windows-x64, macos-arm64, linux-x64
        std::map<std::string, std::string> vars; // ${NAME} in the command templates
        fs::path exe; // the built game
        bool macos = false; // the kit's platform
        bool cross = false; // a Windows kit driven from another OS (a developer check)
    };

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

    fs::file_time_type TimeOf(const fs::path& path) { return fs::last_write_time(path); }

    // One argument per line, ${NAME} replaced; a line that is exactly ${OBJECTS} becomes the object list.
    std::vector<std::string> ExpandTemplate(
        const Build& build, const char* name, const std::vector<std::string>& objects)
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
            run.poll = [&]() {
                std::error_code error;
                uint64_t size = fs::exists(part, error) ? fs::file_size(part, error) : 0;
                Progress(size >> 20, Compiler.size >> 20);
                return false;
            };
#ifdef _WIN32
            fs::path curl = PathFromUtf8(GetEnv("SystemRoot")) / "System32" / "curl.exe";
            int result = Run({ fs::exists(curl) ? Slashes(curl) : "curl.exe", "-fL", "--retry", "3", "--retry-delay",
                                 "5", "-o", Slashes(part.filename()), url },
                run);
#else
            // curl, else wget (a desktop has one of them; neither needs root).
            int result =
                Run({ "curl", "-fL", "--retry", "3", "--retry-delay", "5", "-o", part.filename().string(), url }, run);
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
            Fail("the kit was built with zig " + build.kit["zig"] + ", this Arkeyan-Builder uses " + ZigVersion);
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
        fs::path stamped =
            fs::is_directory(build.options.iso, error) ? build.options.iso / "gamedata.manifest" : build.options.iso;
        auto size = fs::file_size(stamped, error);
        auto time =
            std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(stamped, error).time_since_epoch())
                .count();
        return "game=" + build.game.id + "\niso=" + Slashes(fs::absolute(build.options.iso)) +
               "\nsize=" + std::to_string(size) + "\ntime=" + std::to_string(static_cast<long long>(time)) + "\n";
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
            games.push_back(ReadGame(id, std::move(values)));
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
        if (const Game* game = FindGame(games, sha))
        {
            build.game = *game;
            Log("game: " + game->name + " (default.xex " + sha.substr(0, 12) + "...)");
            return;
        }
        std::string supported;
        for (const Game& game : games)
            supported += (supported.empty() ? "" : ", ") + game.name;
        Fail("this disc is not one of the games these tools build (" + supported +
             "): update the game tools (default.xex SHA-256 " + sha + ")");
    }

    void ReadDisc(Build& build)
    {
        Step("disc", "Reading the disc");
        fs::path disc = build.work / "disc";
        fs::path input = build.work / "shaders" / "input";
        fs::path stamp = disc / "stamp.txt";
        std::string expected = DiscStamp(build);
        bool fromArchive = build.game.shaders.rfind("igz:", 0) == 0;
        size_t unsupported = 0;
        // Every extracted input must survive; one surviving container is not a complete extraction.
        std::string inventory = DiscShaderInventory(input);
        if (ReadText(stamp) == expected && fs::exists(disc / "default.xex") && !inventory.empty() &&
            ReadText(disc / "shaders.txt") == inventory)
        {
            if (auto warning = ReadText(disc / "warnings.txt"); !warning.empty())
                Emit("warn", warning);
            Log("up to date");
            return;
        }
        fs::remove(stamp);
        fs::create_directories(disc);
        // A changed disc or incomplete extraction also invalidates its captured run-time shaders.
        fs::remove_all(input);
        fs::remove_all(build.work / "movies");
        fs::create_directories(input);

        DiscFileSystem fileSystem;
        if (!fileSystem.Open(Slashes(build.options.iso).c_str()))
            Fail("not an Xbox 360 disc image: " + Slashes(build.options.iso));
        std::vector<uint8_t> xex = fileSystem.ReadAll("default.xex");
        if (xex.empty())
            Fail("the disc has no default.xex: is it " + build.game.name + "?");
        WriteBytes(disc / "default.xex", xex.data(), xex.size());

        if (fromArchive && build.game.shaders != "igz:all")
        {
            // Alchemy: every shader is in one IGA archive (games/giants/docs/shaders.md, games/swapforce/docs/shaders.md).
            std::string path = build.game.shaders.substr(4);
            std::vector<uint8_t> archive = fileSystem.ReadAll(path);
            Progress(1, 3);
            ShaderExtraction extraction;
            if (archive.empty() || !ExtractArchiveShaders(archive, input, extraction))
                Fail("the disc has no readable " + path + ": is it " + build.game.name + "?");
            Log(path + ": " + std::to_string(extraction.entries) + " entries, " + std::to_string(extraction.igz) +
                " IGZ, " + std::to_string(extraction.vertex + extraction.pixel) + " shaders (" +
                std::to_string(extraction.vertex) + " vertex, " + std::to_string(extraction.pixel) + " pixel)");
            unsupported += extraction.unsupported;
            if (extraction.vertex + extraction.pixel == 0)
                Fail("no shader found in " + path + ": is this the right disc?");
            Progress(3, 3);
        }
        else if (fromArchive)
        {
            // Trap Team scans the .arc/.bld pairs across the disc, deduplicating shared containers.
            std::vector<std::string> files;
            for (const auto& file : fileSystem.Files())
                files.push_back(file.first);
            auto paths = ShaderArchivePaths(build.game.shaders, files);
            if (paths.empty())
                Fail("no shader archive found: is this " + build.game.name + "?");
            // Decoding the archives (LZMA) is the long part: one per job at once, then written in disc order, so the
            // files, the counts and the log are those of a one-by-one pass.
            struct Decoded
            {
                bool ready = false, ok = false;
                ShaderExtraction extraction;
                std::vector<ArchiveShader> shaders;
            };
            std::vector<Decoded> decoded(paths.size());
            std::mutex mutex;
            std::condition_variable readyChanged;
            std::atomic<size_t> next = 0;
            std::atomic<bool> stop = false;
            std::vector<std::thread> workers;
            struct JoinOnExit // a failure below must not leave a thread running
            {
                std::atomic<bool>& stop;
                std::vector<std::thread>& workers;
                ~JoinOnExit()
                {
                    stop = true;
                    for (auto& worker : workers)
                        worker.join();
                }
            } joinOnExit{ stop, workers };
            for (unsigned t = 0; t < std::min<size_t>(build.options.jobs, paths.size()); t++)
            {
                workers.emplace_back([&]() {
                    for (size_t i; !stop && (i = next++) < paths.size();)
                    {
                        Decoded result;
                        try
                        {
                            std::vector<uint8_t> archive = fileSystem.ReadAll(paths[i]);
                            result.ok =
                                !archive.empty() && CollectArchiveShaders(archive, result.extraction, result.shaders);
                        }
                        catch (const std::exception&)
                        {
                            result.ok = false;
                        }
                        std::lock_guard lock(mutex);
                        decoded[i] = std::move(result);
                        decoded[i].ready = true;
                        readyChanged.notify_all();
                    }
                });
            }
            std::set<std::string> seen;
            size_t total = 0;
            for (size_t i = 0; i < paths.size(); i++)
            {
                const auto& path = paths[i];
                Decoded current;
                {
                    std::unique_lock lock(mutex);
                    readyChanged.wait(lock, [&]() { return decoded[i].ready; });
                    current = std::move(decoded[i]);
                }
                if (!current.ok)
                    Fail("the disc has no readable " + path + ": is it " + build.game.name + "?");
                ShaderExtraction& extraction = current.extraction;
                if (!WriteArchiveShaders(current.shaders, input, extraction, seen))
                    Fail("cannot write the shaders of " + path);
                Log(path + ": " + std::to_string(extraction.entries) + " entries, " + std::to_string(extraction.igz) +
                    " IGZ, " + std::to_string(extraction.vertex + extraction.pixel) + " shaders (" +
                    std::to_string(extraction.vertex) + " vertex, " + std::to_string(extraction.pixel) + " pixel)");
                unsupported += extraction.unsupported;
                total += extraction.vertex + extraction.pixel;
                Progress(i + 1, paths.size());
            }
            if (total == 0)
                Fail("no shader found in " + build.game.shaders + ": is this the right disc?");
            Log(std::to_string(total) + " distinct disc shaders");
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
            Log("default.xex: " + std::to_string(xex.size() >> 10) + " KB, image " + std::to_string(image.size >> 10) +
                " KB");
            Progress(4, 4);
        }
        std::string warning;
        if (unsupported)
            warning = std::to_string(unsupported) +
                      " unreadable or unsupported archive entries; some shaders may be missing. Update the game tools.";
        WriteText(disc / "warnings.txt", warning);
        if (!warning.empty())
            Emit("warn", warning);
        WriteText(disc / "shaders.txt", DiscShaderInventory(input));
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
                               Sha256File(build.options.kit / "bin" / (std::string("XenonRecomp") + Exe)) +
                               "\ncontext=" + Sha256File(build.options.kit / "share" / "ppc_context.h") +
                               "\nkit_config=" + build.kit["ppc_config_sha256." + build.game.id] +
                               "\nkit_context=" + build.kit["ppc_context_sha256"] + "\n";
        if (ReadText(stamp) == expected)
        {
            Log("up to date");
            return;
        }

        if (build.cross)
            Fail("a cross build reuses ppc/ and the shader cache of a native run (docs/tools/arkeyan-builder.md)");
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
        int result = Run({ Slashes(build.options.kit / "bin" / (std::string("XenonRecomp") + Exe)),
                             "config/" + build.game.toml, "ppc_context.h" },
            run);
        if (result != 0 || !fs::exists(xenon / "ppc" / "ppc_config.h"))
        {
            LogTail(run.log);
            Fail("XenonRecomp failed (exit code " + std::to_string(result) + ")");
        }

        // The kit's runtime was compiled against these two headers: another disc version would not match it.
        for (const char* header : { "ppc_config.h", "ppc_context.h" })
        {
            std::string key =
                std::string(header) == "ppc_config.h" ? "ppc_config_sha256." + build.game.id : "ppc_context_sha256";
            if (Sha256File(xenon / "ppc" / header) != build.kit[key])
                Fail(std::string("this disc does not match the kit (") + header + " differs): the kit's " +
                     build.game.name + " runtime was built for another version of the game");
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

    std::string RunStartHint(const std::string& program) { return StartErrorHint(RunStartError(), program); }

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

    void RequireRuntimeTranslations(const Build& build, const fs::path& cache)
    {
        const auto hashes = ShaderCacheHashes(cache);
        for (const auto& [name, path] : RuntimeShaders(build.work / "shaders" / "input" / "runtime"))
            if (!hashes.count(strtoull(path.stem().string().c_str(), nullptr, 16)))
                Fail("required run-time shader " + name +
                     " was not translated; install incomplete, retry with updated game tools");
    }

    void ShaderWarnings(const Build& build)
    {
        std::ifstream log(build.work / "logs" / "xenosrecomp.log");
        unsigned skipped = 0, withoutDxil = 0;
        for (std::string line; std::getline(log, line);)
        {
            skipped += line.rfind("Skipped ", 0) == 0;
            withoutDxil += line.rfind("No DXIL for ", 0) == 0;
        }
        if (skipped)
            Emit("warn", std::to_string(skipped) + " disc shaders could not be translated; some pictures may be "
                                                   "missing. Update the game tools and retry.");
        if (withoutDxil && build.platform == "windows-x64")
            Emit("warn", std::to_string(withoutDxil) +
                             " shaders have no D3D12 version; use Vulkan in video settings or update the game tools.");
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
        // Only the streams the game reads are compiled: the AIR on macOS, the SPIR-V on Linux, the DXIL and the SPIR-V
        // on Windows. SKY_WINDOWS_SHADERS=1 adds Windows' two on another OS, for a cross build of a Windows kit from this
        // run's cache (a developer check); a player's install never sets it.
        bool windows = build.platform == "windows-x64";
        std::string streams = windows ? "dxil,spirv" : build.macos ? "air" : "spirv";
        if (!windows && GetEnv("SKY_WINDOWS_SHADERS") == "1")
            streams = build.macos ? "dxil,spirv,air" : "dxil,spirv";
        std::string expected =
            DiscStamp(build) + "runtime=" + RuntimeShaderList(build) + "\ntranslator=" + Sha256File(xenos) +
            "\ncommon=" + Sha256File(build.options.kit / "share" / "shader_common.h") + "\nstreams=" + streams + "\n";
        if (ReadText(stamp) == expected && fs::exists(cache))
        {
            RequireRuntimeTranslations(build, cache);
            ShaderWarnings(build);
            Log("up to date");
            return;
        }
        if (build.cross)
        {
            std::string text = ReadText(cache);
            if (text.empty() || text.find("g_dxilCacheDecompressedSize = 0;") != std::string::npos ||
                text.find("g_spirvCacheDecompressedSize = 0;") != std::string::npos)
                Fail("a cross build reuses ppc/ and the shader cache of a native run made with SKY_WINDOWS_SHADERS=1 "
                     "(docs/tools/arkeyan-builder.md)");
            Log("the native run's shader cache, with the Windows shaders");
            return;
        }
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
            fs::remove_all(shaders / "cache");
            WriteText(cacheStamp, cacheId);
        }

        // XenosRecomp prints "Progress <done> <total>" (flushed) as it goes: the step's progress bar.
        std::pair<uint64_t, uint64_t> shown{ 0, 0 };
        RunOptions run;
        run.cwd = build.work;
        run.log = build.work / "logs" / "xenosrecomp.log";
        run.timeoutSeconds = 30 * 60;
        run.poll = [&]() {
            std::ifstream log(run.log, std::ios::binary | std::ios::ate);
            if (!log)
                return false;
            std::streamoff size = log.tellg();
            std::string tail(size_t(std::min<std::streamoff>(size, 4096)), '\0');
            log.seekg(size - std::streamoff(tail.size()));
            log.read(tail.data(), std::streamsize(tail.size()));
            size_t at = tail.rfind("Progress ");
            unsigned long long done = 0, total = 0;
            if (at != std::string::npos && sscanf(tail.c_str() + at, "Progress %llu %llu", &done, &total) == 2 &&
                total > 0 && (done != shown.first || total != shown.second))
            {
                shown = { done, total };
                Progress(done, total);
            }
            return false;
        };
        fs::remove(shaders / "shader_cache.cpp"); // a successful child must produce this run's output
        int result = Run({ Slashes(xenos), "shaders/input", "shaders/shader_cache.cpp", "shaders/shader_common.h",
                             "shaders/cache", streams },
            run);
        if (result != 0 || !fs::exists(shaders / "shader_cache.cpp"))
        {
            LogTail(run.log);
            Fail("XenosRecomp failed (exit code " + std::to_string(result) + ")" + RunStartHint("XenosRecomp"));
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
        RequireRuntimeTranslations(build, shaders / "shader_cache.cpp");
        if (ShaderCacheHashes(shaders / "shader_cache.cpp").empty())
            Fail("XenosRecomp produced no translated shaders; install incomplete");
        ShaderWarnings(build);
        CopyIfDifferent(shaders / "shader_cache.cpp", cache);
        fs::remove(shaders / "shader_cache.cpp");
        WriteText(stamp, expected);
    }

    // --- compile and link ------------------------------------------------------------------------------------------

    // zig's copies of what it just built (Linux): Arkeyan-Builder keeps its own objects in obj/.
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
            bool cache = name == "shader_cache";
            // The shader cache is compiled from a copy with its bytes as string literals (shader_cache_literals.h).
            build.vars["SRC"] = cache ? "obj/shader_cache_literals.cpp" : "ppc/" + Slashes(source.filename());
            build.vars["OBJ"] = "obj/" + name + ".o";
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
                if (cache)
                    WriteText(obj / "shader_cache_literals.cpp", ShaderCacheLiterals(ReadText(source)));
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
            threads.emplace_back([&]() {
                while (!failed)
                {
                    size_t index = next++;
                    if (index >= jobs.size())
                        return;
                    const Job& job = jobs[index];
                    try
                    {
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
                    catch (const Failure& problem)
                    {
                        std::lock_guard lock(failureMutex);
                        if (!failed.exchange(true))
                            failure = problem.message;
                    }
                    catch (const std::exception& problem)
                    {
                        std::lock_guard lock(failureMutex);
                        if (!failed.exchange(true))
                            failure = problem.what();
                    }
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        ClearCompilerCache(build);
        std::error_code error;
        fs::remove(obj / "shader_cache_literals.cpp", error);
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

    // One disc-derived icon family for the executable, window, bundle and desktop shortcuts.
    // All sizes are rendered from the same cropped source with the same margin and transparency.
    struct GameIconFiles
    {
        fs::path png, ico, icns;
    };

    GameIconFiles GameIcon(const Build& build)
    {
        fs::path dir = build.work / "icon";
        GameIconFiles files{ build.options.out / "game_icon.png", build.options.out / "game_icon.ico",
            build.options.out / "game_icon.icns" };
        fs::path keyFile = dir / "game_icon.key";
        std::string key = DiscStamp(build) + "recipe=2\n";
        if (fs::exists(files.png) && fs::exists(files.ico) && fs::exists(files.icns) && ReadText(keyFile) == key)
            return files;
        fs::create_directories(dir);
        DiscFileSystem fileSystem;
        if (!fileSystem.Open(Slashes(build.options.iso).c_str()))
            Fail("not an Xbox 360 disc image: " + Slashes(build.options.iso));
        art::Image logo;
        if (!art::ReadLogo(build.game.id, art::ReadFrom(fileSystem), logo) || FitLogoIcon(logo, 16).empty())
        {
            auto png = ReadTitleMetadata(fileSystem.ReadAll("default.xex")).iconPng;
            int width = 0, height = 0, channels = 0;
            if (auto* pixels = stbi_load_from_memory(png.data(), int(png.size()), &width, &height, &channels, 4))
            {
                logo.width = uint32_t(width);
                logo.height = uint32_t(height);
                logo.rgba.resize(size_t(width) * height);
                memcpy(logo.rgba.data(), pixels, logo.rgba.size() * 4);
                stbi_image_free(pixels);
            }
            Log("icon: no readable logo, fitting the disc's title icon with the same recipe");
        }
        auto png = FitLogoIcon(logo, 512), ico = LogoIco(logo), icns = LogoIcns(logo);
        if (png.empty() || ico.empty() || icns.empty())
        {
            // Do not leave a previous disc's icon behind.
            for (const auto& file : { files.png, files.ico, files.icns, keyFile })
                fs::remove(file);
            Log("icon: no readable logo or title icon on the disc; platform default icons will be used");
            return {};
        }
        fs::create_directories(build.options.out);
        WriteBytes(files.png, png.data(), png.size());
        WriteBytes(files.ico, ico.data(), ico.size());
        WriteBytes(files.icns, icns.data(), icns.size());
        WriteText(keyFile, key);
        Log("icon: the disc's art, fitted into one icon family for every platform");
        return files;
    }

    fs::path BundleIcon(const Build& build)
    {
        auto files = GameIcon(build);
        return files.icns.empty() ? build.options.kit / "res" / "ArkeyanRuntime.icns" : files.icns;
    }

    // Windows: the executable's resources, which windres (llvm-mingw's) turns into an object linked with the game: its
    // VERSIONINFO and the complete disc-derived .ico family. Returns
    // the object's path relative to the build folder. A resource compiler failure stops the build.
    std::string ExecutableResources(Build& build)
    {
        fs::path dir = build.work / "icon";
        fs::path object = dir / "game_icon.o";
        fs::path script = dir / "game_icon.rc";
        fs::create_directories(dir);
        std::string rc =
            VersionResource(build.game.name, build.game.runtime, build.kit["game_version." + build.game.id]);
        GameIconFiles files = GameIcon(build);
        std::vector<uint8_t> ico;
        if (files.png.empty())
            Emit("warn", "icon: the disc has no icon, the executable gets none");
        else
        {
            std::string bytes = ReadText(files.ico);
            ico.assign(bytes.begin(), bytes.end());
            rc = "1 ICON \"game_icon.ico\"\n" + rc;
        }
        std::error_code error;
        if (fs::exists(object) && fs::exists(script) && ReadText(script) == rc &&
            (ico.empty() || fs::last_write_time(object, error) >= fs::last_write_time(files.ico, error)))
            return "icon/game_icon.o";
        if (!ico.empty())
            WriteBytes(dir / "game_icon.ico", ico.data(), ico.size());
        WriteBytes(script, rc.data(), rc.size());
        fs::path windres =
            fs::path(build.vars.at("CXX")).parent_path() / (std::string("x86_64-w64-mingw32-windres") + Exe);
        RunOptions run;
        run.cwd = dir;
        run.log = build.work / "logs" / "icon.log";
        if (Run({ Slashes(windres), "--output-format=coff", "--output=game_icon.o", "game_icon.rc" }, run) != 0 ||
            !fs::exists(object))
        {
            LogTail(run.log);
            Emit("warn", "icon: could not embed the game's resources, the executable gets no icon and no version");
            return "";
        }
        if (!ico.empty())
            Log("icon: the game's own");
        return "icon/game_icon.o";
    }

    void Link(Build& build)
    {
        Step("link", "Linking the game");
        fs::path res = build.options.kit / "res";
        const std::string& name = build.game.runtime; // passed as one argument, including spaces and apostrophes
        fs::path app = build.options.out / (name + ".app");
        fs::create_directories(build.options.out);
        GameIcon(build); // Linux needs the same window and shortcut icons too.
        if (build.macos)
        {
            fs::path contents = app / "Contents";
            fs::create_directories(contents / "MacOS");
            fs::create_directories(contents / "Resources");
            CopyIfDifferent(res / build.game.id / "Info.plist", contents / "Info.plist");
            CopyIfDifferent(BundleIcon(build), contents / "Resources" / "ArkeyanRuntime.icns"); // the plist names it
            if (fs::exists(build.options.out / "game_icon.png"))
                CopyIfDifferent(build.options.out / "game_icon.png", contents / "Resources" / "game_icon.png");
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
        if (build.platform == "linux-x64")
            WriteText(build.options.out / "game.desktop",
                GameDesktopEntry(build.game.name, Slashes(build.exe), Slashes(build.options.out / "game_icon.png")));
        std::vector<std::string> objects = ObjectList(build);
        if (build.platform == "windows-x64")
            if (std::string resources = ExecutableResources(build); !resources.empty())
                objects.push_back(resources);
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
        if (!fresh)
        {
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
        }
        // Resource copies above can change an up-to-date bundle too: seal its final icon before retiring the old app.
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
        skybuild::RemoveLegacyRuntime(build.game, build.options.out,
            build.macos                       ? ".app"
            : build.platform == "windows-x64" ? ".exe"
                                              : "");
        WriteText(linkKey, key);
        Log(fresh ? "up to date" : Slashes(build.exe) + ": " + std::to_string(fs::file_size(build.exe) >> 20) + " MB");
    }

    // --- movie shaders ---------------------------------------------------------------------------------------------

    // Hashes of the shader cache's entries ("\t{ 0x<hash>, ...").
    std::set<uint64_t> CachedShaders(const Build& build)
    {
        return ShaderCacheHashes(build.work / "ppc" / "shader_cache.cpp");
    }

    // The game builds the Bink movie shaders at run time (they are in no archive): run it headless for a few seconds
    // with SKY_DUMP_SHADERS, keep the containers the cache lacks, then rebuild the cache with them.
    bool AddMovieShaders(Build& build)
    {
        Step("movies", "Adding the movie shaders");
        fs::path movies = build.work / "movies";
        fs::path stamp = movies / "done.txt";
        const Game& game = build.game;
        if (game.movieShaders == 0)
            Emit("warn",
                "the kit has no run-time shader target; completeness cannot be verified. Update the game tools.");
        fs::path runtime = build.work / "shaders" / "input" / "runtime";
        const auto retained = RuntimeShaders(runtime);
        const size_t kept = retained.size();
        if (fs::exists(stamp) && MoviePassDone(ReadText(stamp), game.movieShaders) && kept >= game.movieShaders &&
            kept > 0)
        {
            Log("up to date");
            return false;
        }
        // A previous capture may be complete even if its rebuild failed. The main flow retries that rebuild first.
        if (game.movieShaders != 0 && kept >= game.movieShaders)
            return false;
        if (build.cross)
        {
            Fail("run-time shaders are incomplete and the game cannot run on this OS; finish the install with the "
                 "native kit");
        }
        // What an earlier pass found stays: an update that repairs a short pass only looks for the rest.
        fs::remove_all(movies);
        fs::create_directories(movies / "dump");
        fs::create_directories(movies / "saves");
        fs::create_directories(movies / "collection");

        // Known: in the cache, or in the disc's shaders but untranslatable (feeding those again gains nothing).
        std::set<uint64_t> cached = CachedShaders(build);
        std::ifstream skipped(build.work / "shaders" / "skipped.txt");
        for (std::string line; std::getline(skipped, line);)
            cached.insert(strtoull(line.c_str(), nullptr, 16));
        auto missing = [&]() {
            std::vector<fs::path> files;
            for (const auto& [name, path] : RuntimeShaders(movies / "dump"))
                if (!retained.count(name) && cached.count(strtoull(path.stem().string().c_str(), nullptr, 16)) == 0)
                    files.push_back(path);
            return files;
        };

        // Nothing of the player's: no settings file, scratch data folder, log, saves and collection, no portal, no sound,
        // no window (SKYLANDERS_DATA moves every folder the game writes, SKY_LOG its log). Test
        // variables a launcher may pass on (screenshots, skipped movies, presses) are removed ("": unset). A game whose
        // run-time shaders come later (game.txt: Swap Force's loading screen, after the menus) gets an empty emulated
        // portal (without one it stops at "PORTAL DISCONNECTED") and presses.
        const std::string portal = game.moviePortal.empty() ? "none" : game.moviePortal;
        const std::pair<const char*, const char*> environment[] = { { "SKY_HEADLESS", "1" },
            { "SKY_DUMP_SHADERS", "dump" }, { "SKY_SETTINGS", "settings.ini" }, { "SKY_SAVE_DIR", "saves" },
            { "SKY_COLLECTION", "collection" }, { "SKY_PORTAL", portal.c_str() }, { "SKY_PORTAL_PLACE", "" },
            { "SDL_AUDIO_DRIVER", "dummy" }, { "SKY_LAUNCHER_SHOTS", "" }, { "SKY_SKIP_MOVIES", "" },
            { "SKY_PRESS", game.moviePress.c_str() }, { "SKYLANDERS_DATA", "data" }, { "SKY_LAUNCHER_INI", "none.ini" },
            { "SKY_LOG", "runtime.log" } };
        std::vector<std::string> previous; // the caller's values, put back after the run
        // Each name's older spellings (SSA_X, SKYLAUNCHER_X...) go too: the game reads them when the current one is unset,
        // and a launcher passes its test variables on under them (a screenshot folder there made the game skip the pass).
        std::vector<std::pair<std::string, std::string>> older; // name, the caller's value
        for (const auto& [name, value] : environment)
        {
            previous.push_back(GetEnv(name));
            SetEnv(name, value);
            std::string first, second;
            skyenv::OlderNames(name, first, second);
            for (const std::string& alias : { first, second })
                if (!alias.empty())
                {
                    older.emplace_back(alias, GetEnv(alias.c_str()));
                    SetEnv(alias.c_str(), "");
                }
        }

        auto start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point firstNew{};
        RunOptions run;
        run.cwd = movies;
        run.log = movies / "game.log";
        run.timeoutSeconds = 120;
        run.poll = [&]() {
            auto now = std::chrono::steady_clock::now();
            Progress(uint64_t(std::chrono::duration_cast<std::chrono::seconds>(now - start).count()), 120);
            size_t found = missing().size();
            if (game.movieShaders != 0)
                return kept + found >= game.movieShaders;
            if (firstNew == std::chrono::steady_clock::time_point{} && found != 0)
                firstNew = now;
            // The movie shaders come together when the first movie starts: give the rest a few seconds.
            return firstNew != std::chrono::steady_clock::time_point{} && now - firstNew > std::chrono::seconds(8);
        };
        Log("running the game headless to collect the shaders it builds at run time");
        int result = Run({ ShortPath(build.exe), ShortPath(build.options.iso) }, run);
        for (size_t i = 0; i < std::size(environment); i++)
            SetEnv(environment[i].first, previous[i]);
        for (const auto& [alias, value] : older)
            SetEnv(alias.c_str(), value);

        std::vector<fs::path> found = missing();
        if (found.empty())
        {
            LogTail(run.log, 15);
            Fail("the movie pass (exit code " + std::to_string(result) +
                 ") produced no new shader; install incomplete, retry the install" + RunStartHint(build.game.runtime));
        }
        fs::create_directories(runtime);
        for (const fs::path& file : found)
            CopyIfDifferent(file, runtime / file.filename());
        size_t total = kept + found.size();
        Log(std::to_string(total) + " shaders built at run time (Bink movies)");
        if (total < game.movieShaders)
        {
            // No stamp: the next install or update tries again.
            Fail("the movie pass collected " + std::to_string(total) + " of " + std::to_string(game.movieShaders) +
                 " required run-time shaders (exit code " + std::to_string(result) +
                 "); install incomplete, retry the install to resume" + RunStartHint(game.runtime));
        }
        // Stamp only after the shaders have been translated, compiled and linked successfully.
        return true;
    }

    // --menu-music: the game's main menu theme, decoded from the disc into a WAV for the launcher's game page
    // (menu_music.h, docs/runtime/host/game_music.md). The kit links the runtime's XMA decoder for it; the launcher has none.
    void MenuMusic(const Build& build)
    {
        Step("music", "Reading the menu music");
        DiscFileSystem fileSystem;
        if (!fileSystem.Open(Slashes(build.options.iso).c_str()))
            Fail("not an Xbox 360 disc image: " + Slashes(build.options.iso));
        std::string error, source;
        std::optional<music::XmaStream> stream =
            music::ReadMenuMusic(build.game.id, art::ReadFrom(fileSystem), error, &source);
        if (!stream)
            Fail("menu music: " + error);
        std::unique_ptr<XmaFrameDecoder> decoder = CreateXmaFrameDecoder();
        if (!decoder)
            Fail("menu music: this Arkeyan-Builder was built without the XMA decoder");
        Log("menu music: " + source + " (" + stream->container + ", " + std::to_string(stream->sampleRate) + " Hz, " +
            std::to_string(stream->channels) + " channels, " + std::to_string(stream->samples) + " samples)");
        std::vector<int16_t> pcm = music::Decode(*decoder, *stream);
        if (pcm.empty())
            Fail("menu music: incomplete or invalid XMA2 stream");
        if (!music::WriteWav(
                build.options.menuMusic, pcm, stream->sampleRate, stream->channels, stream->loopStart, stream->loopEnd))
            Fail("menu music: cannot write " + Slashes(build.options.menuMusic));
    }

    void Usage()
    {
        fprintf(stderr, "usage: Arkeyan-Builder --iso <disc image> --out <game dir> [--kit <kit dir>] [--jobs N] "
                        "[--toolchain <dir>] [--icon-only]\n"
                        "       Arkeyan-Builder --iso <disc image> --menu-music <file.wav> [--kit <kit dir>]\n");
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
        else if (arg == "--icon-only")
            options.iconOnly = true;
        else if (arg == "--menu-music" && hasValue)
            options.menuMusic = PathFromUtf8(argv[++i]);
        else if (arg == "--jobs" && hasValue)
            options.jobs = unsigned(std::max(1, atoi(argv[++i])));
        else
        {
            Usage();
            return 2;
        }
    }
    if (options.iso.empty() || (options.out.empty() && options.menuMusic.empty()))
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
        if (!options.out.empty())
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

        if (!options.menuMusic.empty())
        {
            options.menuMusic = fs::absolute(options.menuMusic);
            IdentifyGame(build);
            MenuMusic(build);
            EndStep();
            Emit("done", Slashes(options.menuMusic));
            return 0;
        }

        // About 2 GB of memory per compiler on the largest files.
        if (options.jobs == 0)
        {
            unsigned threads = std::max(1u, std::thread::hardware_concurrency());
            unsigned memory = unsigned(PhysicalMemoryBytes() / (2ull << 30));
            options.jobs = std::max(1u, std::min(threads, memory));
        }

        build.work = WorkFolder(options.out);
        fs::create_directories(build.work / "logs");
        build.vars["KIT"] = Slashes(options.kit);
        build.vars["PPC"] = "ppc";
        Log("Arkeyan-Builder " + build.kit["version"] + " (" + build.kit["build"] + ", " + build.platform + "), kit " +
            Slashes(options.kit));

        std::error_code error;
        auto space = fs::space(options.out, error);
        if (!error && space.available < (6ull << 30))
            Emit("warn", "only " + std::to_string(space.available >> 20) +
                             " MB free on the game drive; about 5 GB "
                             "are needed");

        IdentifyGame(build);
        if (options.iconOnly)
        {
            Step("icon", "Making the game's icon");
            GameIconFiles files = GameIcon(build);
            EndStep();
            Emit("done", Slashes(files.png));
            return 0;
        }
        build.vars["GAME"] = build.game.id;
        PrepareToolchain(build);
        ReadDisc(build);
        TranslateCode(build);
        TranslateShaders(build);
        Compile(build);
        Link(build);
        if (AddMovieShaders(build))
        {
            // A failure here is the install's: without these shaders a game can lose its movies, its interface or its whole
            // picture. A rerun retries it (the shader step's stamp holds the run-time list).
            TranslateShaders(build);
            Compile(build);
            Link(build);
        }
        RequireRuntimeTranslations(build, build.work / "ppc" / "shader_cache.cpp");
        fs::create_directories(build.work / "movies");
        WriteText(build.work / "movies" / "done.txt",
            std::to_string(RuntimeShaders(build.work / "shaders" / "input" / "runtime").size()) + "\n");
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
