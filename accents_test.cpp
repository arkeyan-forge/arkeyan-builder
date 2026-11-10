// Path and process contracts used by Arkeyan-Builder, with synthetic files only.
#define main AccentsBuilderMain
#include "main.cpp"
#undef main
#include <cstdio>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#endif

int TestAccentedBuildPaths(int argc, char** argv)
{
#ifdef _WIN32
    printf("accents: ACP=%u OEMCP=%u\n", GetACP(), GetOEMCP());
#endif
    if (argc > 1 && std::string(argv[1]) == "--accents-codepage")
        return 0;
    if (argc > 1 && std::string(argv[1]) == "--accents-child")
    {
        if (argc != 3 || GetEnv("SKY_ACCENTS_TEST") != argv[2])
            return 3;
        std::ofstream out(PathFromUtf8(argv[2]), std::ios::binary);
        out << "synthetic path probe\n";
        out.close();
        return out.fail() ? 4 : 0;
    }

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            fprintf(stderr, "FAIL accents Arkeyan-Builder: %s\n", what);
            failures++;
        }
    };
    fs::path scratch = fs::current_path() / ".scratch" / "ArkeyanBuilderAccentsTest";
    fs::create_directories(scratch);
    std::string previous = GetEnv("SKY_ACCENTS_TEST");
    for (const auto* name : { u8"données-é", u8"データ" })
    {
        fs::path folder = scratch / fs::path(name);
        fs::create_directories(folder);
#ifdef _WIN32
        fs::path self = ExecutableDirectory() / "ArkeyanVersionResourceTest.exe";
#else
        fs::path self = ExecutableDirectory() / "ArkeyanVersionResourceTest";
#endif
        fs::path child = folder / self.filename();
        fs::copy_file(self, child, fs::copy_options::overwrite_existing);
        const std::string filename = Slashes(fs::path(name) / "argument with spaces.txt");
        SetEnv("SKY_ACCENTS_TEST", filename);
        check(GetEnv("SKY_ACCENTS_TEST") == filename, "UTF-8 environment round trip");
        fs::create_directories(folder / fs::path(name));
        RunOptions options;
        options.cwd = folder;
        options.log = folder / "child.log";
        options.timeoutSeconds = 15;
        check(Run({ Slashes(child), "--accents-child", filename }, options) == 0,
            "Unicode executable, cwd, arguments and log");
        fs::path output = folder / PathFromUtf8(filename);
        check(fs::is_regular_file(output), "child wrote the exact Unicode path");
        fs::path reference = folder / "reference.txt";
        std::ofstream(reference, std::ios::binary) << "synthetic path probe\n";
        check(!Sha256File(output).empty() && Sha256File(output) == Sha256File(reference), "hash reads Unicode path");
        size_t found = 0;
        for (const auto& entry : fs::directory_iterator(output.parent_path()))
            found += entry.path().filename() == output.filename();
        check(found == 1, "list the exact child output");
        fs::path response = folder / "compile.rsp";
        const std::vector<std::string> args{ "clang++", "-I" + Slashes(folder), filename, "a\"b", "c\\d" };
        auto command = WithResponseFile(args, response);
        check(command == std::vector<std::string>{ "clang++", "@" + Slashes(response) }, "response-file argument");
        check(ReadText(response) == "\"-I" + Slashes(folder) + "\"\n\"" + filename + "\"\n\"a\\\"b\"\n\"c\\\\d\"\n",
            "response file preserves UTF-8 and quoting");
        check(fs::remove(response), "delete response file");
        const std::vector<std::string> zig{ "zig", "c++", "-o", "/o'brien/Skylanders Spyro's Adventure", "-L/k'it" };
        check(WithResponseFile(zig, response) == zig && !fs::exists(response), "zig keeps apostrophes on argv");
        std::string compiler = GetEnv("SKY_ACCENTS_COMPILER");
        if (compiler.empty())
            compiler = SKY_ACCENTS_COMPILER;
        if (compiler.empty())
            printf("accents: compiler execution unverified (cross build; set SKY_ACCENTS_COMPILER)\n");
        else
        {
            fs::path source = folder / "probe.cpp";
            fs::path header = folder / "probe.h";
            std::ofstream(source) << "#include \"probe.h\"\nint main() { return accent_value; }\n";
            std::ofstream(header) << "constexpr int accent_value = 0;\n";
            fs::path program = folder / "compiled-probe.exe";
            std::vector<std::string> compileArgs{ compiler, "-I" + Slashes(folder), Slashes(source), "-o",
                Slashes(program) };
#ifdef _WIN32
            compileArgs.push_back("-static");
#endif
            auto compile = WithResponseFile(compileArgs, response);
            check(Run(compile, options) == 0, "compiler consumes UTF-8 response file and source path");
            check(fs::is_regular_file(program), "compiler wrote the exact Unicode output path");
            check(Run({ Slashes(program) }, options) == 0, "run compiled synthetic program");
        }
        check(fs::remove(output), "delete child output");
        fs::remove_all(folder);
        check(!fs::exists(folder), "delete Unicode work folder");
    }
    SetEnv("SKY_ACCENTS_TEST", previous);
    fs::remove(scratch);
    return failures;
}
