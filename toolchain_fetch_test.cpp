// ExtractZip on archives built here (no network, no game data): stored and deflated entries, directories, and every
// refusal a hostile or damaged archive must get.
#include "toolchain_fetch.h"
#include <miniz.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

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

static void Put16(std::string& out, uint32_t v) { out += char(v & 255); out += char(v >> 8 & 255); }
static void Put32(std::string& out, uint32_t v) { Put16(out, v & 0xFFFF); Put16(out, v >> 16); }

struct Item
{
    std::string name, data;
    bool deflate = false;
    bool badCrc = false;
    uint32_t madeBy = 0, attributes = 0;
};

static std::string MakeZip(const std::vector<Item>& items)
{
    std::string body, directory;
    for (const Item& item : items)
    {
        std::string packed = item.data;
        if (item.deflate)
        {
            size_t size = 0;
            void* heap = tdefl_compress_mem_to_heap(item.data.data(), item.data.size(), &size, 128);
            packed.assign(static_cast<char*>(heap), size);
            mz_free(heap);
        }
        uint32_t crc = uint32_t(mz_crc32(mz_crc32(0, nullptr, 0), reinterpret_cast<const uint8_t*>(item.data.data()), item.data.size()));
        if (item.badCrc)
            crc ^= 1;
        uint32_t method = item.deflate ? 8 : 0, offset = uint32_t(body.size());
        Put32(body, 0x04034b50); Put16(body, 20); Put16(body, 0); Put16(body, method); Put32(body, 0);
        Put32(body, crc); Put32(body, uint32_t(packed.size())); Put32(body, uint32_t(item.data.size()));
        Put16(body, uint32_t(item.name.size())); Put16(body, 0);
        body += item.name + packed;
        Put32(directory, 0x02014b50); Put16(directory, 20 | item.madeBy << 8); Put16(directory, 20); Put16(directory, 0);
        Put16(directory, method); Put32(directory, 0); Put32(directory, crc); Put32(directory, uint32_t(packed.size()));
        Put32(directory, uint32_t(item.data.size())); Put16(directory, uint32_t(item.name.size())); Put16(directory, 0);
        Put16(directory, 0); Put16(directory, 0); Put16(directory, 0); Put32(directory, item.attributes);
        Put32(directory, offset);
        directory += item.name;
    }
    std::string end;
    Put32(end, 0x06054b50); Put16(end, 0); Put16(end, 0); Put16(end, uint32_t(items.size()));
    Put16(end, uint32_t(items.size())); Put32(end, uint32_t(directory.size())); Put32(end, uint32_t(body.size()));
    Put16(end, 0);
    return body + directory + end;
}

static std::string Read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

static std::string Pseudo(size_t size)
{
    std::string text(size, '\0');
    uint32_t state = 12345;
    for (char& c : text)
    {
        state = state * 1664525 + 1013904223;
        c = char(state >> 24);
    }
    return text;
}

// Extracts `zip` into a fresh folder; the result and the error text.
static bool Extract(const fs::path& root, const std::string& zip, const std::string& name, std::string& error)
{
    fs::path archive = root / (name + ".zip"), folder = root / name;
    std::ofstream(archive, std::ios::binary) << zip;
    uint64_t last = 0;
    bool ok = ExtractZip(archive, folder, [&](uint64_t done, uint64_t) { last = done; }, error);
    (void)last;
    return ok;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    fs::path root = fs::absolute(argv[1]);
    fs::remove_all(root);
    fs::create_directories(root);
    std::string error;

    // Entries bigger than the 32 KB inflate window and the 64 KB read block, so the streaming loops wrap.
    std::string text;
    for (int i = 0; text.size() < 300000; ++i)
        text += "line " + std::to_string(i) + " of a compressible file\n";
    std::string noise = Pseudo(200000);
    std::string good = MakeZip({ { "pkg/", "", false }, { "pkg/bin/", "", false }, { "pkg/bin/text.txt", text, true },
        { "pkg/bin/noise.bin", noise, true }, { "pkg/stored.bin", noise, false }, { "pkg/empty", "", false },
        { "pkg\\win.txt", "backslash", false } });
    CHECK(Extract(root, good, "good", error));
    CHECK(error.empty());
    CHECK(Read(root / "good/pkg/bin/text.txt") == text);
    CHECK(Read(root / "good/pkg/bin/noise.bin") == noise);
    CHECK(Read(root / "good/pkg/stored.bin") == noise);
    CHECK(fs::exists(root / "good/pkg/empty") && fs::file_size(root / "good/pkg/empty") == 0);
    CHECK(Read(root / "good/pkg/win.txt") == "backslash");

    // Nothing may land outside the folder.
    for (const char* name : { "../evil", "a/../../evil", "/abs", "C:/evil", "a/../../../evil" })
    {
        CHECK(!Extract(root, MakeZip({ { name, "x", false } }), "slip", error));
        CHECK(!error.empty());
        CHECK(!fs::exists(root / "evil") && !fs::exists(root.parent_path() / "evil"));
    }
    CHECK(!Extract(root, MakeZip({ { "ok", "x", true, true } }), "crc", error));
    CHECK(error.find("ok") != std::string::npos);
    CHECK(!Extract(root, MakeZip({ { "link", "target", false, false, 3, 0120777u << 16 } }), "link", error));
    CHECK(error.find("link") != std::string::npos);
    CHECK(Extract(root, MakeZip({ { "file", "x", false, false, 3, 0100644u << 16 } }), "unixfile", error));
    CHECK(!Extract(root, good.substr(0, good.size() / 2), "cut", error));
    CHECK(!Extract(root, "not a zip archive at all", "junk", error));
    CHECK(!Extract(root, std::string(), "empty", error));
    CHECK(!ExtractZip(root / "missing.zip", root / "missing", [](uint64_t, uint64_t) {}, error));

    // The real compiler archive, when given (SKY_LLVM_ZIP): extracted, with its compiler present.
    if (const char* real = getenv("SKY_LLVM_ZIP"))
    {
        CHECK(ExtractZip(real, root / "real", [](uint64_t, uint64_t) {}, error));
        CHECK(fs::exists(root / "real/llvm-mingw-20260922-ucrt-x86_64/bin/x86_64-w64-mingw32-clang++.exe"));
    }

    fs::remove_all(root);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
