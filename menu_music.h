#pragma once

// The game's main menu theme from the player's disc, for the launcher's game page (docs/runtime/host/game_music.md):
// found through the game's manifest (games/<id>/art.json, "music"), decoded with the runtime's XMA frame decoder
// (apu/xma_decoder.h: FFmpeg's WMA Pro decoder, LGPL) and written as a 16-bit PCM WAV with its loop. Nothing is
// shipped: the file exists only on the player's machine, made from their own disc, like the game's pictures.

#include "host/art/game_art.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class XmaFrameDecoder;

namespace music
{
    // One XMA2 cue as stored: 2048-byte packets, mono/stereo or interleaved stereo stems (up to ten channels).
    struct XmaStream
    {
        std::string container;           // "wwise" (RIFF/RIFX WAVE, Wwise's .wem) or "fsb5" (FMOD sample bank)
        uint32_t sampleRate = 0, channels = 0;
        uint64_t samples = 0;            // samples per channel to play (the container's length)
        uint64_t loopStart = 0, loopEnd = 0; // the container's loop [start, end), end 0: none (the whole track loops)
        std::vector<uint8_t> packets;
    };

    // [Wwise] A .wem: a RIFF (little-endian) or RIFX (big-endian) WAVE whose "fmt " is XMA2 (0x166) and whose "data" are
    // the packets. The length is XMA2's PlayLength, the loop its LoopBegin and LoopLength.
    std::optional<XmaStream> ParseWwise(std::span<const uint8_t> file);
    // [FMOD] An FSB5 bank of one XMA sample (mode 10): length and loop from its header, channel extension for stems.
    std::optional<XmaStream> ParseFsb5(std::span<const uint8_t> file);

    // [Wwise] A file package (AKPK, Lux's audio\streaming.pck): where the stream `id` is, from its header (the first
    // `HeaderSize` bytes and the stream table after them). Big-endian (Xbox 360) or little-endian.
    struct PackageEntry
    {
        uint64_t offset = 0, size = 0;
    };
    constexpr size_t PackageHeaderPeek = 16;
    // The header's total size (the bytes ReadPackageEntry needs), from its first PackageHeaderPeek bytes; 0 if not an AKPK.
    size_t PackageHeaderSize(std::span<const uint8_t> peek);
    std::optional<PackageEntry> ReadPackageEntry(std::span<const uint8_t> header, uint32_t id);

    // [Alchemy] Whether an archive entry's name is the manifest's `entry`: the fragment ends one of the name's parts
    // ("INTER_Results_Screen" is "...\INTER_Results_Screen", not "...\INTER_Results_Screen_Alt"; "mu_title_main_loop.wav"
    // is ".../mu_title_main_loop.wav/0x5d6ebbbd.wav.hz.wav.enc"), any case.
    bool EntryMatches(const std::string& name, const std::string& entry);

    // The theme of `game` (its manifest's "music") on `disc`; nullopt and `error` if the game names none or the disc
    // lacks it. `source` gets where it was found ("audio/streaming.pck:0x0C45FC64").
    std::optional<XmaStream> ReadMenuMusic(
        std::string_view game, const art::Disc& disc, std::string& error, std::string* source = nullptr);

    // Decodes the stream frame by frame (the hardware's way: 512 samples per frame and channel) into interleaved 16-bit
    // samples, `stream.samples` per channel. The decoder's first frame is its priming (the codec gives silence for it,
    // and the containers' lengths do not count it): dropped, so sample 0 is the first sample of the music.
    std::vector<int16_t> Decode(XmaFrameDecoder& decoder, const XmaStream& stream);

    // A 16-bit PCM WAV (RIFF, little-endian) of `pcm`, interleaved, with a "smpl" chunk giving the loop [loopStart,
    // loopEnd) (the whole file when loopEnd is 0). Written as `<path>.part`, then renamed.
    bool WriteWav(const std::filesystem::path& path, std::span<const int16_t> pcm, uint32_t sampleRate,
        uint32_t channels, uint64_t loopStart, uint64_t loopEnd);
}
