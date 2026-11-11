// Real install steps with invented inputs. Every child call is intercepted; no program or game is launched.
#define Run GuardRun
#define RunStartError GuardStartError
#define main SkyBuildMain
#include "main.cpp"
#undef main
#undef RunStartError
#undef Run
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

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

static std::function<int(const std::vector<std::string>&, const RunOptions&)> child;
static unsigned long startError = 0;
int GuardRun(const std::vector<std::string>& args, const RunOptions& options)
{
    startError = 0;
    if (!child)
        throw std::runtime_error("unexpected child request");
    return child(args, options);
}
unsigned long GuardStartError() { return startError; }

static void Put(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    WriteText(path, text);
}

static void Container(const fs::path& path)
{
    std::string bytes(0x48, '\0');
    bytes[0] = 0x10;
    bytes[1] = 0x2A;
    bytes[2] = 0x11;
    bytes[3] = path.extension() == ".vs" ? 1 : 0;
    bytes[7] = 0x40;
    bytes[11] = 8;
    Put(path, bytes);
}

static std::string Failed(const std::function<void()>& action)
{
    try
    {
        action();
    }
    catch (const Failure& failure)
    {
        return failure.message;
    }
    CHECK(false);
    return {};
}

static std::pair<int, std::string> CaptureMain(std::vector<std::string> args)
{
    std::vector<char*> argv;
    for (auto& arg : args)
        argv.push_back(arg.data());
    FILE* capture = tmpfile();
    if (!capture)
        throw std::runtime_error("cannot capture test output");
    fflush(stdout);
    int saved = dup(fileno(stdout));
    if (saved < 0 || dup2(fileno(capture), fileno(stdout)) < 0)
        throw std::runtime_error("cannot redirect test output");
    int status = SkyBuildMain(int(argv.size()), argv.data());
    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    rewind(capture);
    std::string text;
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof(buffer), capture)) != 0;)
        text.append(buffer, n);
    fclose(capture);
    return { status, text };
}

static Build Fixture(const fs::path& root)
{
    Build build;
    build.options.out = root / "output";
    build.options.kit = root / "kit";
    build.options.iso = root / "disc";
    build.work = build.options.out / "ssabuild-work";
    build.game = ReadGame("synthetic", { { "runtime", "SyntheticRuntime" }, { "movie_shaders", "2" } });
    build.platform = "windows-x64";
    fs::create_directories(build.work / "logs");
    fs::create_directories(build.work / "ppc");
    Put(build.options.iso / "gamedata.manifest", "synthetic\n");
    Put(build.options.iso / "default.xex", "invented bytes, no executable code\n");
    Put(build.options.kit / "bin" / (std::string("XenosRecomp") + Exe), "synthetic translator\n");
    Put(build.options.kit / "share" / "shader_common.h", "synthetic common\n");
    Put(build.work / "shaders/input/disc.ps", "synthetic disc input\n");
    build.exe = build.options.out / "SyntheticRuntime";
    return build;
}

static void TestInventory(Build& build)
{
    fs::path input = build.work / "shaders/input";
    std::string inventory = DiscShaderInventory(input);
    CHECK(!inventory.empty());
    Container(input / "runtime/0000000000000001.ps");
    CHECK(DiscShaderInventory(input) == inventory);
    CHECK(RuntimeShaders(input / "runtime").size() == 1);
    Put(input / "runtime/0000000000000002.ps", "partial");
    Container(input / "runtime/wrong.ps");
    Container(input / "runtime/0000000000000003.vs");
    fs::resize_file(input / "runtime/0000000000000003.vs", 12);
    CHECK(RuntimeShaders(input / "runtime").size() == 1);
    Put(input / "disc.ps", "changed\n");
    CHECK(DiscShaderInventory(input) != inventory);
    fs::remove(input / "disc.ps");
    CHECK(DiscShaderInventory(input).empty());
    fs::remove_all(input / "runtime");
    Put(input / "disc.ps", "restored\n");
}

static void TestTranslation(Build& build)
{
    fs::path runtime = build.work / "shaders/input/runtime";
    Container(runtime / "0000000000000001.ps");
    child = [&](const auto&, const RunOptions&) {
        startError = 225;
        return -1;
    };
    std::string error = Failed([&] { TranslateShaders(build); });
    CHECK(error.find("Windows Security blocked XenosRecomp") != std::string::npos);
    CHECK(!fs::exists(build.work / "shaders/stamp.txt"));
    // A successful translator may still have dropped the required shader.
    child = [&](const auto&, const RunOptions& run) {
        Put(run.cwd / "shaders/shader_cache.cpp", "\t{ 0x2, 0 },\n");
        Put(run.log, "Skipped 0000000000000001\nNo DXIL for 0000000000000002\n");
        return 0;
    };
    CHECK(Failed([&] { TranslateShaders(build); }).find("was not translated") != std::string::npos);
    CHECK(!fs::exists(build.work / "shaders/stamp.txt"));
    child = [&](const auto&, const RunOptions& run) {
        Put(run.cwd / "shaders/shader_cache.cpp", "\t{ 0x1, 0 },\n");
        Put(run.log, "Skipped 0000000000000002\nNo DXIL for 0000000000000001\n");
        return 0;
    };
    TranslateShaders(build);
    CHECK(fs::exists(build.work / "shaders/stamp.txt"));
    child = {};
    TranslateShaders(build); // cached install still verifies the retained shader
    Put(build.work / "ppc/shader_cache.cpp", "\t{ 0x2, 0 },\n");
    CHECK(Failed([&] { TranslateShaders(build); }).find("was not translated") != std::string::npos);
    fs::remove_all(runtime);
    fs::remove(build.work / "shaders/stamp.txt");
    child = [&](const auto&, const RunOptions& run) {
        Put(run.cwd / "shaders/shader_cache.cpp", "// no entries\n");
        return 0;
    };
    CHECK(Failed([&] { TranslateShaders(build); }).find("no translated shaders") != std::string::npos);
}

static void TestMovies(Build& build)
{
    fs::path runtime = build.work / "shaders/input/runtime";
    fs::path stamp = build.work / "movies/done.txt";
    Put(build.work / "ppc/shader_cache.cpp", "\t{ 0x99, 0 },\n");
    Put(build.work / "shaders/skipped.txt", "");
    Put(stamp, "1\n");
    child = [&](const auto&, const RunOptions& run) {
        Container(run.cwd / "dump/0000000000000001.ps");
        CHECK(!run.poll());
        return -2;
    };
    CHECK(Failed([&] { AddMovieShaders(build); }).find("1 of 2") != std::string::npos);
    CHECK(RuntimeShaders(runtime).size() == 1);
    CHECK(!fs::exists(stamp));
    // Resume keeps the old container; the poll target includes it, and a duplicate contributes nothing.
    SetEnv("SKY_PRESS", "test-previous");
    child = [&](const auto&, const RunOptions& run) {
        CHECK(GetEnv("SKY_PRESS").empty());
        Container(run.cwd / "dump/0000000000000001.ps");
        CHECK(!run.poll());
        Container(run.cwd / "dump/0000000000000002.vs");
        CHECK(run.poll());
        return -3;
    };
    CHECK(AddMovieShaders(build));
    CHECK(GetEnv("SKY_PRESS") == "test-previous");
    SetEnv("SKY_PRESS", "");
    CHECK(RuntimeShaders(runtime).size() == 2);
    CHECK(!fs::exists(stamp)); // capture alone cannot commit completion
    child = {};
    CHECK(!AddMovieShaders(build)); // complete capture retries the rebuild without another child
    Put(stamp, "2\n");
    CHECK(!AddMovieShaders(build));
    fs::remove(runtime / "0000000000000002.vs");
    child = [&](const auto&, const RunOptions&) {
        startError = 226;
        return -1;
    };
    std::string error = Failed([&] { AddMovieShaders(build); });
    CHECK(error.find("install incomplete") != std::string::npos);
    CHECK(error.find("Windows Security blocked SyntheticRuntime") != std::string::npos);
    CHECK(!fs::exists(stamp));
    build.game.movieShaders = 0;
    CHECK(Failed([&] { AddMovieShaders(build); }).find("install incomplete") != std::string::npos);
    build.game.movieShaders = 2;
    build.cross = true;
    CHECK(Failed([&] { AddMovieShaders(build); }).find("native kit") != std::string::npos);
    build.cross = false;
}

static void TestMainFailure(Build& build)
{
    // Let main use cached synthetic disc inputs, then fake code translation and a blocked shader tool.
    fs::remove_all(build.work / "shaders/input/runtime");
    fs::remove(build.work / "shaders/stamp.txt");
    const std::string xex = ReadText(build.options.iso / "default.xex");
#ifdef _WIN32
    const std::string platform = "windows-x64";
#elif defined(__APPLE__)
    const std::string platform = "macos-arm64";
#else
    const std::string platform = "linux-x64";
#endif
    Put(build.options.kit / "kit.txt", "games=synthetic\nplatform=" + platform + "\nzig=0.17.0\n");
    Put(build.options.kit / "config/synthetic/game.txt",
        "runtime=SyntheticRuntime\nxex_sha256=" + Sha256Text(xex) + "\nmovie_shaders=2\ntoml=synthetic.toml\n");
    Put(build.options.kit / "config/synthetic/synthetic.toml", "synthetic config\n");
    Put(build.options.kit / "bin" / (std::string("XenonRecomp") + Exe), "synthetic translator\n");
    Put(build.options.kit / "share/ppc_context.h", "synthetic context\n");
    build.kit = ReadKeyValues(build.options.kit / "kit.txt");
    const std::string header = "synthetic header\n";
    Put(build.options.kit / "kit.txt", ReadText(build.options.kit / "kit.txt") + "ppc_config_sha256.synthetic=" +
                                           Sha256Text(header) + "\nppc_context_sha256=" + Sha256Text(header) + "\n");
    Put(build.work / "disc/default.xex", xex);
    Put(build.work / "disc/stamp.txt", DiscStamp(build));
    Put(build.work / "disc/shaders.txt", DiscShaderInventory(build.work / "shaders/input"));
    fs::path toolchain = build.options.out / "toolchain";
#if defined(_WIN32) || defined(__linux__)
    Put(toolchain / Compiler.compiler, "synthetic compiler\n");
#endif
    child = [&](const std::vector<std::string>& args, const RunOptions& run) {
        if (args[0] == "xcrun")
        {
            Put(run.log, "synthetic SDK\n");
            return 0;
        }
        if (fs::path(args[0]).stem() == "XenonRecomp")
        {
            Put(run.cwd / "ppc/ppc_config.h", header);
            Put(run.cwd / "ppc/ppc_context.h", header);
            return 0;
        }
        CHECK(fs::path(args[0]).stem() == "XenosRecomp");
        startError = 225;
        return -1;
    };
    auto [status, output] =
        CaptureMain({ "Arkeyan-Builder", "--iso", Slashes(build.options.iso), "--out", Slashes(build.options.out),
            "--kit", Slashes(build.options.kit), "--toolchain", Slashes(toolchain), "--jobs", "2" });
    CHECK(status == 1);
    CHECK(output.find("@error XenosRecomp failed") != std::string::npos);
    CHECK(output.find("Windows Security blocked XenosRecomp") != std::string::npos);
    CHECK(output.find("@done") == std::string::npos);
    CHECK(!fs::exists(build.work / "movies/done.txt"));
}

static void TestExecutableResources(Build& build)
{
    build.vars["CXX"] = Slashes(build.options.kit / "compiler/bin/clang++");
    build.kit["game_version." + build.game.id] = "1.2.3";
    // Cache invented icon files so this check reads no disc artwork and only exercises resource compilation.
    for (const char* extension : { "png", "ico", "icns" })
        Put(build.options.out / (std::string("game_icon.") + extension), "synthetic icon\n");
    Put(build.work / "icon/game_icon.key", DiscStamp(build) + "recipe=2\n");
    fs::path object = build.work / "icon/game_icon.o";
    Put(object, "old resource\n");
    int calls = 0;
    child = [&](const std::vector<std::string>& args, const RunOptions& run) {
        CHECK(fs::path(args[0]).stem().string().find("windres") != std::string::npos);
        ++calls;
        if (calls == 1)
        {
            Put(run.cwd / "game_icon.o", "partial resource\n");
            return 1;
        }
        if (calls == 3)
            Put(run.cwd / "game_icon.o", "complete resource\n");
        return 0;
    };
    CHECK(Failed([&] { ExecutableResources(build); }).find("executable resources") != std::string::npos);
    CHECK(!fs::exists(object));
    // Success without producing an object is also an error; neither failure may become a cached success.
    CHECK(Failed([&] { ExecutableResources(build); }).find("executable resources") != std::string::npos);
    CHECK(!fs::exists(object));
    CHECK(ExecutableResources(build) == "icon/game_icon.o");
    CHECK(ExecutableResources(build) == "icon/game_icon.o");
    CHECK(calls == 3);
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    try
    {
        fs::path root = fs::absolute(argv[1]);
        fs::remove_all(root);
        Build build = Fixture(root);
        TestInventory(build);
        TestTranslation(build);
        TestMovies(build);
        TestMainFailure(build);
        TestExecutableResources(build);
        fs::remove_all(root);
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "test exception: %s\n", error.what());
        return 1;
    }
    catch (const Failure& error)
    {
        fprintf(stderr, "unexpected install failure: %s\n", error.message.c_str());
        return 1;
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
