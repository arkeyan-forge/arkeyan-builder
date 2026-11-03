// ArkeyanBuilderMusicTest: the menu music's readers (menu_music.h) on synthetic files; with SSA_ISO / GIANTS_ISO
// (read only), each game's theme (SSA, Giants, Swap Force, Trap Team) found on its disc, decoded, and written as a looping WAV.
#include "apu/xma_decoder.h"
#include "menu_music.h"
#include "vfs.h"
#include "host/art/alchemy_archive.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int failures = 0;

static void Put32(std::vector<uint8_t>& out, uint32_t v, bool big)
{
    for (int i = 0; i < 4; i++)
        out.push_back(uint8_t(big ? v >> (24 - 8 * i) : v >> (8 * i)));
}
static void Put16(std::vector<uint8_t>& out, uint16_t v, bool big)
{
    out.push_back(uint8_t(big ? v >> 8 : v));
    out.push_back(uint8_t(big ? v : v >> 8));
}
static void Tag(std::vector<uint8_t>& out, const char* text) { out.insert(out.end(), text, text + 4); }

// A WAVE with an XMA2 "fmt " (52 bytes) and `packets` 2048-byte packets.
static std::vector<uint8_t> Wem(bool big, uint32_t packets, uint32_t length, uint32_t loopBegin, uint32_t loopLength)
{
    std::vector<uint8_t> f;
    Tag(f, big ? "RIFX" : "RIFF");
    Put32(f, 0, big);
    Tag(f, "WAVE");
    Tag(f, "fmt ");
    Put32(f, 52, big);
    Put16(f, 0x166, big);
    Put16(f, 2, big);
    Put32(f, 44100, big);
    Put32(f, 0, big);
    Put16(f, 0, big);
    Put16(f, 0, big);
    Put16(f, 34, big);
    Put16(f, 1, big);
    Put32(f, 3, big);
    Put32(f, 0, big);
    Put32(f, 0, big);
    Put32(f, 0, big);
    Put32(f, length, big);
    Put32(f, loopBegin, big);
    Put32(f, loopLength, big);
    Put32(f, 0, big);
    Tag(f, "data");
    Put32(f, packets * 2048, big);
    f.resize(f.size() + packets * 2048, 0);
    return f;
}

// A deterministic decoder: only synthetic frame markers are used, never encoded game audio.
class MarkerDecoder : public XmaFrameDecoder
{
public:
    void Reset() override {}
    bool DecodeFrame(const uint8_t* data, uint32_t offset, uint32_t count, uint32_t rate, uint32_t channels,
        int16_t* samples) override
    {
        if (offset != 0 || count != 18 || rate != 48000 || channels != 2)
            return false;
        const int marker = ((data[1] & 1) << 1) | (data[2] >> 7);
        std::fill_n(samples, SamplesPerFrame * channels, int16_t((marker + 1) * 1000));
        return true;
    }
};

static void Frame(std::vector<uint8_t>& bytes, size_t bit, uint32_t marker)
{
    const uint32_t frame = (18u << 3) | (marker << 1); // length, two marker bits, zero footer
    for (uint32_t i = 0; i < 18; i++)
        if (frame & (1u << (17 - i)))
            bytes[(bit + i) / 8] |= uint8_t(0x80 >> ((bit + i) % 8));
}

static void TestPackets()
{
    MarkerDecoder decoder;
    music::XmaStream stereo;
    stereo.channels = 2;
    stereo.sampleRate = 48000;
    stereo.samples = 512;
    stereo.packets.resize(2048);
    stereo.packets[0] = 2 << 2;
    Frame(stereo.packets, 32, 0);
    Frame(stereo.packets, 50, 1);
    auto pcm = music::Decode(decoder, stereo);
    CHECK(pcm.size() == 1024 && pcm.front() == 2000 && pcm.back() == 2000);
    stereo.samples = 48000; // an incomplete stream must fail instead of padding seconds of silence
    CHECK(music::Decode(decoder, stereo).empty());

    music::XmaStream adaptive;
    adaptive.channels = 4;
    adaptive.sampleRate = 48000;
    adaptive.samples = 1024;
    adaptive.packets.resize(7 * 2048);
    const uint32_t order[] = { 0, 1, 3, 4, 5, 6 };
    const uint8_t skips[] = { 3, 1, 2, 0, 255, 255 };
    const uint32_t markers[] = { 0, 3, 3, 1, 2, 3 };
    for (size_t i = 0; i < std::size(order); i++)
    {
        const size_t at = order[i] * 2048;
        adaptive.packets[at] = 1 << 2;
        adaptive.packets[at + 3] = skips[i];
        Frame(adaptive.packets, at * 8 + 32, markers[i]);
    }
    pcm = music::Decode(decoder, adaptive);
    CHECK(pcm.size() == 4096);
    if (pcm.size() == 4096)
    {
        CHECK(pcm[0] == 2000 && pcm[1] == 2000 && pcm[2] == 4000 && pcm[3] == 4000);
        CHECK(pcm[2048] == 3000 && pcm[2050] == 4000);
    }
    adaptive.packets[2 * 2048] = 1; // an unclaimed packet is legal only when entirely zero padding
    CHECK(music::Decode(decoder, adaptive).empty());
    adaptive.packets[2 * 2048] = 0;
    adaptive.packets[3] = 0; // two stream chains collide
    CHECK(music::Decode(decoder, adaptive).empty());

    // FSB5's channel extension overrides the base channel field (adaptive stereo stems).
    std::vector<uint8_t> fsb;
    Tag(fsb, "FSB5");
    Put32(fsb, 1, false);
    Put32(fsb, 1, false);
    Put32(fsb, 13, false);
    Put32(fsb, 0, false);
    Put32(fsb, 2048, false);
    Put32(fsb, 10, false);
    fsb.resize(60);
    const uint64_t field = (uint64_t(1024) << 34) | (9 << 1) | 1;
    Put32(fsb, uint32_t(field), false);
    Put32(fsb, uint32_t(field >> 32), false);
    Put32(fsb, (1 << 25) | (1 << 1), false);
    fsb.push_back(10);
    fsb.resize(fsb.size() + 2048);
    auto parsed = music::ParseFsb5(fsb);
    CHECK(parsed && parsed->channels == 10 && parsed->sampleRate == 48000 && parsed->samples == 1024);
    fsb.resize(65);
    CHECK(!music::ParseFsb5(fsb));
}

static void TestSynthetic()
{
    for (bool big : { false, true })
    {
        std::vector<uint8_t> f = Wem(big, 3, 100000, 1000, 50000);
        auto s = music::ParseWwise(f);
        CHECK(s.has_value());
        if (!s)
            continue;
        CHECK(s->channels == 2 && s->sampleRate == 44100 && s->samples == 100000);
        CHECK(s->loopStart == 1000 && s->loopEnd == 51000 && s->packets.size() == 3 * 2048);
        CHECK(!music::ParseWwise(std::span<const uint8_t>(f.data(), 20)));
    }
    CHECK(music::EntryMatches("a\\b\\INTER_Results_Screen", "inter_results_screen"));
    CHECK(!music::EntryMatches("a\\b\\INTER_Results_Screen_Alt", "INTER_Results_Screen"));
    CHECK(music::EntryMatches("x/mu_title_main_loop.wav/0x5d6ebbbd.wav.hz.wav.enc", "mu_title_main_loop.wav"));

    // An AKPK, big-endian: header, languages (4), banks (4), streams (4 + one 24-byte entry).
    std::vector<uint8_t> p;
    Tag(p, "AKPK");
    Put32(p, 16 + 4 + 4 + 28, true);
    Put32(p, 1, true);
    Put32(p, 4, true);
    Put32(p, 4, true);
    Put32(p, 28, true);
    Put32(p, 0, true);
    Put32(p, 0, true);
    Put32(p, 1, true);
    Put32(p, 0xABCD, true);
    Put32(p, 2048, true);
    Put32(p, 0, true);
    Put32(p, 5000, true);
    Put32(p, 3, true);
    Put32(p, 0, true);
    CHECK(music::PackageHeaderSize(p) == p.size());
    auto entry = music::ReadPackageEntry(p, 0xABCD);
    CHECK(entry && entry->offset == 3 * 2048 && entry->size == 5000);
    CHECK(!music::ReadPackageEntry(p, 1));

    // The WAV: a "smpl" chunk with the loop, a clamped one when out of range.
    std::vector<int16_t> pcm(2000, 7);
    std::filesystem::path path = std::filesystem::temp_directory_path() / "ArkeyanBuilderMusicTest.wav";
    CHECK(music::WriteWav(path, pcm, 44100, 2, 100, 600));
    FILE* file = fopen(path.string().c_str(), "rb");
    std::vector<uint8_t> w(8192);
    w.resize(file ? fread(w.data(), 1, w.size(), file) : 0);
    if (file)
        fclose(file);
    CHECK(w.size() == 12 + 24 + 68 + 8 + 4000 && memcmp(w.data(), "RIFF", 4) == 0 && memcmp(&w[36], "smpl", 4) == 0);
    CHECK(w.size() > 100 && w[88] == 100 && w[92] == (599 & 255) &&
          w[93] == (599 >> 8)); // smpl body (44) +44: loop start, +48: end
    std::filesystem::remove(path);
}

static void TestDisc(const char* game, const char* iso, uint64_t minimumSeconds)
{
    DiscFileSystem disc;
    CHECK(disc.Open(iso));
    std::string error, source;
    auto stream = music::ReadMenuMusic(game, art::ReadFrom(disc), error, &source);
    if (!stream)
        printf("  %s: %s\n", game, error.c_str());
    CHECK(stream.has_value());
    if (!stream)
        return;
    std::unique_ptr<XmaFrameDecoder> decoder = CreateXmaFrameDecoder();
    CHECK(decoder != nullptr);
    if (!decoder)
        return;
    std::vector<int16_t> pcm = music::Decode(*decoder, *stream);
    double sum = 0;
    int peak = 0;
    size_t clipped = 0;
    for (int16_t v : pcm)
    {
        sum += double(v) * v;
        peak = std::max(peak, std::abs(int(v)));
        clipped += std::abs(int(v)) >= 32767;
    }
    double rms = pcm.empty() ? 0 : std::sqrt(sum / double(pcm.size()));
    printf("  %s: %s (%s, %u Hz, %u ch, %.1f s, loop %llu..%llu), rms %.0f peak %d\n", game, source.c_str(),
        stream->container.c_str(), stream->sampleRate, stream->channels, double(stream->samples) / stream->sampleRate,
        (unsigned long long)stream->loopStart, (unsigned long long)stream->loopEnd, rms, peak);
    CHECK(stream->samples >= minimumSeconds * stream->sampleRate);
    CHECK(pcm.size() == stream->samples * stream->channels);
    CHECK(rms > 300 && rms < 16000 && peak > 3000);
    CHECK(clipped < pcm.size() / 1000); // occasional source peaks are valid; sustained clipping is not
    // The last two seconds must contain music too: resizing an incomplete decode used to hide a half-silent file.
    double tail = 0;
    const size_t tailSamples = std::min(pcm.size(), size_t(stream->sampleRate) * stream->channels * 2);
    for (size_t i = pcm.size() - tailSamples; i < pcm.size(); i++)
        tail += double(pcm[i]) * pcm[i];
    CHECK(tailSamples > 0 && std::sqrt(tail / std::max<size_t>(1, tailSamples)) > 100);
    std::filesystem::path out =
        std::filesystem::temp_directory_path() / (std::string("ArkeyanBuilderMusicTest_") + game + ".wav");
    CHECK(music::WriteWav(out, pcm, stream->sampleRate, stream->channels, stream->loopStart, stream->loopEnd));
    if (getenv("SKY_KEEP_MUSIC") == nullptr)
        std::filesystem::remove(out);
    else
        printf("  kept %s\n", out.string().c_str());
}

static void TestAdaptiveDisc(const char* iso)
{
    DiscFileSystem disc;
    CHECK(disc.Open(iso));
    const auto* entry = disc.Find("archives/inter_streamed_bank.pak");
    CHECK(entry != nullptr);
    if (!entry)
        return;
    auto archive = art::alchemy::IgaArchive::Open(
        [&](uint64_t offset, size_t size, uint8_t* out) {
            return disc.Read(*entry, offset, out, uint32_t(size)) == size;
        },
        entry->size);
    CHECK(archive.has_value());
    if (!archive)
        return;
    auto decoder = CreateXmaFrameDecoder();
    CHECK(decoder != nullptr);
    if (!decoder)
        return;
    size_t tested = 0;
    for (size_t i = 0; i < archive->Count(); i++)
    {
        auto stream = music::ParseFsb5(archive->Read(i));
        CHECK(stream.has_value());
        if (!stream || stream->channels <= 2)
            continue;
        auto pcm = music::Decode(*decoder, *stream);
        if (pcm.size() != stream->samples * stream->channels)
            printf("  adaptive decode failed: %s\n", archive->Name(i).c_str());
        CHECK(pcm.size() == stream->samples * stream->channels);
        double sum = 0;
        for (int16_t value : pcm)
            sum += double(value) * value;
        CHECK(!pcm.empty() && std::sqrt(sum / std::max<size_t>(1, pcm.size())) > 100);
        tested++;
    }
    CHECK(tested == 102);
    printf("  Swap Force adaptive samples: %zu decoded\n", tested);
}

int main()
{
    TestSynthetic();
    TestPackets();
    if (const char* iso = getenv("SSA_ISO"); iso && *iso)
        TestDisc("ssa", iso, 20);
    if (const char* iso = getenv("GIANTS_ISO"); iso && *iso)
        TestDisc("giants", iso, 20);
    if (const char* iso = getenv("SWAPFORCE_ISO"); iso && *iso)
    {
        TestDisc("swapforce", iso, 20);
        TestAdaptiveDisc(iso);
    }
    if (const char* iso = getenv("TRAPTEAM_ISO"); iso && *iso)
        TestDisc("trapteam", iso, 20);
    printf(failures == 0 ? "ArkeyanBuilderMusicTest: all passed\n" : "ArkeyanBuilderMusicTest: %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}
