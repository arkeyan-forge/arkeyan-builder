// Function fingerprints for the edition finder (docs/tools/edition_finder.md): a disc whose game code differs from every
// supported release may still hold the same functions at other addresses. Each address of games/<id>/addresses.h is
// fingerprinted from our own disc at kit time (config/<id>/fingerprints.txt) and searched for in the player's loaded image.
// A fingerprint is hashes only, never instruction bytes: FNV-1a over the function's instruction words with the fields a
// link changes masked (b/bl targets, lis immediates and the low halves that pair with them), the hash of its first four
// words with every D-form immediate masked (to find candidates fast), and the same for each function it calls (the call
// graph tells apart functions whose own code is the same).
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace skybuild::fingerprint
{
    // A loaded image (big-endian PowerPC) and its code sections.
    struct Code
    {
        uint32_t base = 0;
        std::vector<uint32_t> words; // the whole image, one word per 4 bytes
        std::vector<std::pair<uint32_t, uint32_t>> sections; // code ranges [start, end), addresses
        uint32_t Word(uint32_t address) const { return words[(address - base) / 4]; }
        bool IsCode(uint32_t address, uint32_t count = 1) const;
    };
    Code FromBytes(
        uint32_t base, const uint8_t* data, size_t size, std::vector<std::pair<uint32_t, uint32_t>> sections);

    struct Callee
    {
        uint32_t words = 0;
        uint64_t hash = 0;
    };

    struct Print
    {
        std::string name; // GAME_HOOK_<Name>, or the constexpr's name
        uint32_t address = 0; // in our release
        bool data = false; // a data address: read from a lis / low-half pair of the function below
        uint32_t words = 0; // the function's length
        uint64_t hash = 0, head = 0;
        std::vector<Callee> callees; // in call order
        uint32_t hi = 0, lo = 0; // data: the instruction indexes of the pair, in the function
        bool inside = false; // a code address inside the function (a call site, a return address): `hi` words in
    };

    // The hashes of the function of `words` words at `start`.
    uint64_t Hash(const Code& code, uint32_t start, uint32_t words);
    uint64_t Head(const Code& code, uint32_t start);
    std::vector<Callee> Callees(
        const Code& code, uint32_t start, uint32_t words, const std::map<uint32_t, uint32_t>& lengths);

    // Every function XenonRecomp made: the sub_XXXXXXXX names in the game's library (the bytes of an .a archive), or one hex
    // address per line. Sorted.
    std::vector<uint32_t> FunctionStarts(const std::string& file);

    // The addresses of games/<id>/addresses.h: `#define GAME_HOOK_<Name> sub_XXXXXXXX` and every constexpr whose value is
    // inside the image (data). Name -> address.
    std::vector<std::pair<std::string, uint32_t>> ReadAddresses(
        const std::string& header, uint32_t base, uint32_t size);

    // Kit time: the fingerprints of `addresses` in our image, with every function's start from XenonRecomp's
    // ppc_func_mapping.cpp (`starts`). A data address takes the first function that refers to it whose hash is unique.
    // `missing` receives the names that got none.
    std::vector<Print> Make(const Code& code, const std::vector<uint32_t>& starts,
        const std::vector<std::pair<std::string, uint32_t>>& addresses, std::vector<std::string>& missing);
    std::string Write(const std::vector<Print>& prints); // config/<id>/fingerprints.txt
    std::vector<Print> Read(const std::string& text);

    struct Found
    {
        std::string name;
        uint32_t ours = 0, theirs = 0; // theirs 0: not found
        std::string confidence; // "sure" (one match), "call graph" (one candidate whose callees all match), "likely"
        // (the best of several by its callees), "ambiguous: N candidates", "not found"
    };
    // The player's image: every fingerprint searched for.
    std::vector<Found> Find(const Code& code, const std::vector<Print>& prints);
}
