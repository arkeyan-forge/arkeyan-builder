// See menu_music.h. Formats: [Wwise] the .wem (RIFF/RIFX WAVE, XMA2WAVEFORMATEX) and the AKPK file package; [FMOD] FSB5;
// [Xbox] XMA2 packets. Described in docs/runtime/host/game_music.md.
#include "menu_music.h"

#include "apu/xma_decoder.h"
#include "host/art/alchemy_archive.h"
#include "host/collection/json.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

extern const unsigned char g_embeddedArtManifests[];
extern const size_t g_embeddedArtManifestsSize;

namespace music
{
    namespace
    {
        constexpr uint32_t PacketSize = 2048;
        constexpr uint32_t PacketHeaderBits = 32;
        constexpr uint32_t PayloadBits = (PacketSize * 8) - PacketHeaderBits; // the frames' bits in one packet
        constexpr uint32_t FrameLengthBits = 15;
        constexpr uint32_t NoFrame = 0x7FFF; // a frame length of all ones: the packet's padding
        constexpr uint16_t FormatXma2 = 0x166;
        constexpr uint32_t Fsb5ModeXma = 10;

        uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
        uint64_t Le64(const uint8_t* p) { return Le32(p) | uint64_t(Le32(p + 4)) << 32; }
        uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

        // Bit `bit` of the frames' stream: the packets' payloads one after the other (their 4-byte headers skipped).
        struct PayloadBitsReader
        {
            const std::vector<uint8_t>& packets;
            uint64_t total = 0; // payload bits

            explicit PayloadBitsReader(const std::vector<uint8_t>& bytes)
                : packets(bytes), total(uint64_t(bytes.size() / PacketSize) * PayloadBits)
            {}

            uint32_t Bit(uint64_t bit) const
            {
                uint64_t packet = bit / PayloadBits, inside = bit % PayloadBits + PacketHeaderBits;
                uint8_t byte = packets[size_t(packet * PacketSize + inside / 8)];
                return (byte >> (7 - inside % 8)) & 1;
            }
            uint32_t Read(uint64_t bit, uint32_t count) const
            {
                uint32_t value = 0;
                for (uint32_t i = 0; i < count; i++)
                    value = value << 1 | Bit(bit + i);
                return value;
            }
        };

        // XMA2 packet header: frame count (6 bits), bit offset of the first frame starting in the packet (15 bits, from
        // the end of the header), metadata (3 bits), packets of other streams to skip (8 bits).
        uint32_t FirstFrameOffset(const uint8_t* packet)
        {
            return ((packet[0] & 3u) << 13) | (packet[1] << 5) | (packet[2] >> 3);
        }

        std::string Lower(std::string text)
        {
            for (char& c : text)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // The manifest's "music" of `game` (games/<id>/art.json, built in with the pictures' manifests).
        collection::Json MusicManifest(std::string_view game)
        {
            collection::Json all;
            std::string error;
            if (!collection::Json::Parse(
                    std::string(reinterpret_cast<const char*>(g_embeddedArtManifests), g_embeddedArtManifestsSize), all,
                    error))
                return {};
            for (const collection::Json& json : all.Items())
                if (json["game"].AsString() == game)
                    return json["music"];
            return {};
        }

        std::vector<uint8_t> ReadRange(const art::Disc& disc, const std::string& path, uint64_t offset, uint64_t size)
        {
            if (size == 0 || size > (64ull << 20))
                return {};
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (!disc.read(path, offset, bytes.size(), bytes.data()))
                return {};
            return bytes;
        }
    }

    bool EntryMatches(const std::string& name, const std::string& entry)
    {
        std::string lower = Lower(name), part = Lower(entry);
        for (size_t at = lower.find(part); !part.empty() && at != std::string::npos; at = lower.find(part, at + 1))
        {
            size_t end = at + part.size();
            if ((at == 0 || lower[at - 1] == '/' || lower[at - 1] == '\\') &&
                (end == lower.size() || lower[end] == '/' || lower[end] == '\\'))
                return true;
        }
        return false;
    }

    std::optional<XmaStream> ParseWwise(std::span<const uint8_t> file)
    {
        if (file.size() < 12 || memcmp(file.data() + 8, "WAVE", 4) != 0)
            return std::nullopt;
        bool big = memcmp(file.data(), "RIFX", 4) == 0;
        if (!big && memcmp(file.data(), "RIFF", 4) != 0)
            return std::nullopt;
        auto u16 = [&](size_t at) {
            return uint16_t(big ? file[at] << 8 | file[at + 1] : file[at] | file[at + 1] << 8);
        };
        auto u32 = [&](size_t at) { return big ? Be32(&file[at]) : Le32(&file[at]); };

        XmaStream stream;
        stream.container = "wwise";
        bool format = false;
        // The chunks; the RIFF size is not trusted (Wwise's streamed files say less than they hold).
        for (size_t at = 12; at + 8 <= file.size();)
        {
            uint32_t size = u32(at + 4);
            size_t body = at + 8;
            if (memcmp(&file[at], "fmt ", 4) == 0)
            {
                // XMA2WAVEFORMATEX: WAVEFORMATEX (18 bytes), then streams, channel mask, samples encoded, bytes per
                // block, play begin, play length, loop begin, loop length, loop count, encoder version, block count.
                if (size < 52 || body + 52 > file.size() || u16(body) != FormatXma2 || u16(body + 18) != 1)
                    return std::nullopt; // not XMA2, or several streams (only one stereo or mono stream is read)
                stream.channels = u16(body + 2);
                stream.sampleRate = u32(body + 4);
                uint32_t playBegin = u32(body + 32), playLength = u32(body + 36);
                uint32_t loopBegin = u32(body + 40), loopLength = u32(body + 44);
                stream.samples = playLength != 0 ? playLength : u32(body + 24);
                if (playBegin != 0)
                    return std::nullopt; // never seen on the discs read: the start would need skipping
                if (loopLength != 0)
                {
                    stream.loopStart = loopBegin;
                    stream.loopEnd = uint64_t(loopBegin) + loopLength;
                }
                format = true;
            }
            else if (memcmp(&file[at], "data", 4) == 0)
            {
                if (!format)
                    return std::nullopt;
                size_t length = std::min<size_t>(size, file.size() - body) / PacketSize * PacketSize;
                stream.packets.assign(file.begin() + body, file.begin() + body + length);
                break;
            }
            at = body + size + (size & 1);
        }
        if (!format || stream.packets.empty() || stream.channels == 0 || stream.channels > 2 || stream.sampleRate == 0)
            return std::nullopt;
        return stream;
    }

    std::optional<XmaStream> ParseFsb5(std::span<const uint8_t> file)
    {
        if (file.size() < 0x3C || memcmp(file.data(), "FSB5", 4) != 0)
            return std::nullopt;
        uint32_t version = Le32(&file[4]), count = Le32(&file[8]), headersSize = Le32(&file[12]);
        uint32_t namesSize = Le32(&file[16]), dataSize = Le32(&file[20]), mode = Le32(&file[24]);
        size_t headerSize = version == 0 ? 0x40 : 0x3C;
        if (version > 1 || count != 1 || mode != Fsb5ModeXma || headersSize < 8 ||
            headerSize + headersSize + namesSize > file.size())
            return std::nullopt;

        // The sample's header: one 64-bit field (more chunks: bit 0; rate index: bits 1-4; channels: bits 5-6; data
        // offset / 32: bits 7-33; samples: bits 34-63), then its chunks.
        static constexpr uint32_t Rates[] = { 4000, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000,
            96000 };
        static constexpr uint32_t Channels[] = { 1, 2, 6, 8 };
        size_t at = headerSize;
        uint64_t field = Le64(&file[at]);
        at += 8;
        XmaStream stream;
        stream.container = "fsb5";
        uint32_t rate = (field >> 1) & 0xF;
        stream.sampleRate = rate < std::size(Rates) ? Rates[rate] : 0;
        stream.channels = Channels[(field >> 5) & 3];
        uint64_t dataOffset = ((field >> 7) & 0x7FFFFFF) * 32;
        stream.samples = field >> 34;
        for (bool more = field & 1; more && at + 4 <= headerSize + headersSize;)
        {
            uint32_t chunk = Le32(&file[at]);
            more = chunk & 1;
            uint32_t size = (chunk >> 1) & 0xFFFFFF, type = chunk >> 25;
            at += 4;
            if (at + size > headerSize + headersSize)
                return std::nullopt;
            if (type == 1 && size >= 1)
                stream.channels = file[at];
            else if (type == 2 && size >= 4)
                stream.sampleRate = Le32(&file[at]);
            else if (type == 3 && size >= 8) // loop start, loop end (inclusive)
            {
                stream.loopStart = Le32(&file[at]);
                stream.loopEnd = uint64_t(Le32(&file[at + 4])) + 1;
            }
            at += size;
        }
        if (dataOffset >= dataSize || stream.samples == 0)
            return std::nullopt;
        size_t data = headerSize + headersSize + namesSize + size_t(dataOffset);
        size_t length = std::min<size_t>(dataSize - dataOffset, file.size() - std::min(file.size(), data)) /
                        PacketSize * PacketSize;
        if (data >= file.size() || length == 0 || stream.channels == 0 || stream.channels > 10 ||
            (stream.channels > 2 && stream.channels % 2 != 0) || stream.sampleRate == 0)
            return std::nullopt;
        stream.packets.assign(file.begin() + data, file.begin() + data + length);
        return stream;
    }

    size_t PackageHeaderSize(std::span<const uint8_t> peek)
    {
        if (peek.size() < PackageHeaderPeek || memcmp(peek.data(), "AKPK", 4) != 0)
            return 0;
        // The header size (after the 8 bytes of magic and size) is in the package's byte order; version 1 tells it.
        bool big = Be32(&peek[8]) == 1;
        uint32_t size = big ? Be32(&peek[4]) : Le32(&peek[4]);
        return size_t(size) + 8;
    }

    std::optional<PackageEntry> ReadPackageEntry(std::span<const uint8_t> header, uint32_t id)
    {
        if (header.size() < 0x18 || PackageHeaderSize(header) > header.size())
            return std::nullopt;
        bool big = Be32(&header[8]) == 1;
        auto u32 = [&](size_t at) { return big ? Be32(&header[at]) : Le32(&header[at]); };
        auto u64 = [&](size_t at) { return big ? Be64(&header[at]) : Le64(&header[at]); };
        // Version, then the sizes of the language map, the sound bank table and the stream table; each table is a
        // count and its entries: { id, block size, file size (64-bit), start block, language }.
        uint32_t languages = u32(12), banks = u32(16), streams = u32(20);
        size_t table = 0x18 + size_t(languages) + banks;
        if (table + 4 > header.size() || table + streams > header.size())
            return std::nullopt;
        uint32_t count = u32(table);
        if (streams < 4 || count == 0 || (streams - 4) / count != 24)
            return std::nullopt; // only the 24-byte entries of the packages read so far
        for (uint32_t i = 0; i < count; i++)
        {
            size_t entry = table + 4 + size_t(i) * 24;
            if (u32(entry) == id)
                return PackageEntry{ uint64_t(u32(entry + 4)) * u32(entry + 16), u64(entry + 8) };
        }
        return std::nullopt;
    }

    std::optional<XmaStream> ReadMenuMusic(
        std::string_view game, const art::Disc& disc, std::string& error, std::string* source)
    {
        collection::Json manifest = MusicManifest(game);
        std::string package = manifest["package"].AsString(), archive = manifest["archive"].AsString();
        std::optional<XmaStream> stream;
        if (!package.empty())
        {
            // [Lux] A Wwise stream of the file package, by its id.
            uint32_t id = uint32_t(strtoul(manifest["stream"].AsString().c_str(), nullptr, 0));
            std::vector<uint8_t> peek = ReadRange(disc, package, 0, PackageHeaderPeek);
            size_t size = PackageHeaderSize(peek);
            std::vector<uint8_t> header = size != 0 ? ReadRange(disc, package, 0, size) : std::vector<uint8_t>{};
            std::optional<PackageEntry> entry = ReadPackageEntry(header, id);
            if (!entry)
            {
                error = "no stream " + manifest["stream"].AsString() + " in " + package;
                return std::nullopt;
            }
            stream = ParseWwise(ReadRange(disc, package, entry->offset, entry->size));
            if (source != nullptr)
                *source = package + ":" + manifest["stream"].AsString();
        }
        else if (!archive.empty())
        {
            // [Alchemy] An entry of an IGA archive: an FMOD sample bank.
            std::string entryName = manifest["entry"].AsString();
            uint64_t size = disc.size(archive);
            auto iga = size != 0 ? art::alchemy::IgaArchive::Open(
                                       [&disc, archive](uint64_t offset, size_t count, uint8_t* out) {
                                           return disc.read(archive, offset, count, out);
                                       },
                                       size)
                                 : std::nullopt;
            if (!iga)
            {
                error = "no archive " + archive;
                return std::nullopt;
            }
            for (size_t i = 0; i < iga->Count() && !stream; i++)
                if (EntryMatches(iga->Name(i), entryName))
                {
                    stream = ParseFsb5(iga->Read(i));
                    if (source != nullptr)
                        *source = archive + ":" + iga->Name(i);
                }
            if (!stream && error.empty())
                error = "no FSB5 entry " + entryName + " in " + archive;
        }
        else
        {
            error = "the game has no menu music in its manifest";
            return std::nullopt;
        }
        if (!stream && error.empty())
            error = "the menu music is not an XMA2 stream this reader knows";
        return stream;
    }

    std::vector<int16_t> Decode(XmaFrameDecoder& decoder, const XmaStream& stream)
    {
        const uint32_t channels = stream.channels;
        if (channels == 0 || channels > 10 || (channels > 2 && channels % 2 != 0) || stream.sampleRate == 0 ||
            stream.samples == 0 || stream.samples > uint64_t(stream.sampleRate) * 1800)
            return {};
        if (channels > 2)
        {
            // The packet's skip count is the number of intervening packets, not the distance to the next one.
            // Follow each stereo stream independently, retaining frame continuations across its packets.
            const size_t packets = stream.packets.size() / PacketSize;
            const uint32_t stems = channels / 2;
            std::vector<int16_t> interleaved(size_t(stream.samples) * channels);
            std::vector<bool> visited(packets, false);
            for (uint32_t stem = 0; stem < stems; stem++)
            {
                XmaStream part = stream;
                part.channels = 2;
                part.packets.clear();
                for (size_t packet = stem; packet < packets;)
                {
                    if (visited[packet])
                        return {};
                    visited[packet] = true;
                    const auto first = stream.packets.begin() + packet * PacketSize;
                    part.packets.insert(part.packets.end(), first, first + PacketSize);
                    packet += size_t(stream.packets[packet * PacketSize + 3]) + 1;
                }
                auto decoded = Decode(decoder, part);
                if (decoded.size() != size_t(stream.samples) * 2)
                    return {};
                for (size_t i = 0; i < stream.samples; i++)
                    for (uint32_t c = 0; c < 2; c++)
                        interleaved[i * channels + stem * 2 + c] = decoded[i * 2 + c];
            }
            for (size_t packet = 0; packet < packets; packet++)
                if (!visited[packet] &&
                    std::any_of(stream.packets.begin() + packet * PacketSize,
                        stream.packets.begin() + (packet + 1) * PacketSize, [](uint8_t byte) { return byte != 0; }))
                    return {}; // zero-filled padding at the end of an FMOD block is not a stream
            return interleaved;
        }
        std::vector<int16_t> out;
        const size_t wanted = size_t(stream.samples) * channels;
        out.reserve(wanted + XmaFrameDecoder::SamplesPerFrame * channels);
        PayloadBitsReader bits(stream.packets);
        std::vector<int16_t> frame(XmaFrameDecoder::SamplesPerFrame * channels);
        std::vector<uint8_t> frameBytes((1u << FrameLengthBits) / 8 + 1);
        decoder.Reset();
        bool first = true;
        for (size_t packet = 0; packet < stream.packets.size() / PacketSize && out.size() < wanted; packet++)
        {
            const uint8_t* header = &stream.packets[packet * PacketSize];
            const uint32_t count = header[0] >> 2;
            uint64_t at = uint64_t(packet) * PayloadBits + FirstFrameOffset(header);
            // XMA2's packet header is authoritative about the number of frame starts. The frame footer alone
            // undercounts these discs, leaving half the decoded file silent. A split frame continues in the next
            // packet's payload; that packet's first-frame offset skips its continuation.
            for (uint32_t index = 0; index < count && out.size() < wanted; index++)
            {
                if (at + FrameLengthBits > bits.total)
                    return {};
                const uint32_t length = bits.Read(at, FrameLengthBits);
                if (length == NoFrame || length <= FrameLengthBits || at + length > bits.total)
                    return {};
                std::fill(frameBytes.begin(), frameBytes.end(), uint8_t(0));
                for (uint32_t i = 0; i < length; i++)
                    if (bits.Bit(at + i))
                        frameBytes[i / 8] |= uint8_t(0x80 >> (i % 8));
                if (!decoder.DecodeFrame(frameBytes.data(), 0, length, stream.sampleRate, channels, frame.data()))
                    return {};
                if (!first)
                    out.insert(out.end(), frame.begin(), frame.end());
                first = false;
                at += length;
            }
        }
        // The last codec frame can be shorter than 512 samples; never mask missing packets with a long silence.
        if (out.size() + XmaFrameDecoder::SamplesPerFrame * channels < wanted)
            return {};
        out.resize(wanted, 0);
        return out;
    }

    bool WriteWav(const std::filesystem::path& path, std::span<const int16_t> pcm, uint32_t sampleRate,
        uint32_t channels, uint64_t loopStart, uint64_t loopEnd)
    {
        if (channels == 0 || sampleRate == 0 || pcm.empty())
            return false;
        uint64_t frames = pcm.size() / channels;
        if (loopEnd == 0 || loopEnd > frames || loopStart >= loopEnd)
            loopStart = 0, loopEnd = frames;
        std::vector<uint8_t> out;
        auto put32 = [&](uint32_t v) {
            for (int i = 0; i < 4; i++)
                out.push_back(uint8_t(v >> (8 * i)));
        };
        auto put16 = [&](uint16_t v) {
            out.push_back(uint8_t(v));
            out.push_back(uint8_t(v >> 8));
        };
        auto tag = [&](const char* text) { out.insert(out.end(), text, text + 4); };
        uint32_t dataSize = uint32_t(pcm.size() * 2);
        tag("RIFF");
        put32(4 + (8 + 16) + (8 + 60) + (8 + dataSize));
        tag("WAVE");
        tag("fmt ");
        put32(16);
        put16(1); // PCM
        put16(uint16_t(channels));
        put32(sampleRate);
        put32(sampleRate * channels * 2);
        put16(uint16_t(channels * 2));
        put16(16);
        // "smpl": the loop, as sample frames (end inclusive), the way samplers and SDL-based players read it.
        tag("smpl");
        put32(60);
        for (int i = 0; i < 7; i++)
            put32(i == 2 ? uint32_t(1000000000ull / sampleRate)
                         : (i == 3 ? 60 : 0)); // manufacturer, product, period, unity note...
        put32(1); // one loop
        put32(0);
        put32(0); // cue id
        put32(0); // forward
        put32(uint32_t(loopStart));
        put32(uint32_t(loopEnd - 1));
        put32(0); // fraction
        put32(0); // play count: forever
        tag("data");
        put32(dataSize);
        size_t header = out.size();
        out.resize(header + dataSize);
        for (size_t i = 0; i < pcm.size(); i++)
        {
            out[header + 2 * i] = uint8_t(uint16_t(pcm[i]));
            out[header + 2 * i + 1] = uint8_t(uint16_t(pcm[i]) >> 8);
        }

        std::filesystem::path part = path;
        part += ".part";
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
        FILE* file = fopen(part.string().c_str(), "wb");
        if (file == nullptr)
            return false;
        bool written = fwrite(out.data(), 1, out.size(), file) == out.size();
        written = fclose(file) == 0 && written;
        if (!written)
        {
            std::filesystem::remove(part, ignored);
            return false;
        }
        std::filesystem::rename(part, path, ignored);
        return !ignored;
    }
}
