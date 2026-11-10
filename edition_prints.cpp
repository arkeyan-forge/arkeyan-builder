// Kit time: the fingerprints of a game's addresses (fingerprint.h) from our own disc, for the kit's config/<id>/.
//   ArkeyanEditionPrints <disc> <functions> <addresses.h> <out fingerprints.txt>
// <functions>: the game's library (private/libs/<id>/libSSARecompLib.a: its sub_XXXXXXXX names are every function
// XenonRecomp made), or one hex address per line. Prints how many addresses got a fingerprint, and that each is found again at its own place.
#include "edition.h"
#include "host/xex_title.h"
#include "vfs.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <image.h>
#include <iterator>
#include <sstream>

int main(int argc, char** argv)
{
    using namespace skybuild;
    if (argc != 5)
    {
        fprintf(stderr, "usage: ArkeyanEditionPrints <disc> <functions> <addresses.h> <out fingerprints.txt>\n");
        return 2;
    }
    DiscFileSystem disc;
    std::vector<uint8_t> xex = disc.Open(argv[1]) ? disc.ReadAll("default.xex") : std::vector<uint8_t>();
    Image image = XexImageReadable(xex) ? Image::ParseImage(xex.data(), xex.size()) : Image{};
    if (image.data == nullptr)
    {
        fprintf(stderr, "error: no readable default.xex on %s\n", argv[1]);
        return 1;
    }
    const fingerprint::Code code = ImageCode(image);
    std::ifstream list(argv[2], std::ios::binary);
    const std::vector<uint32_t> starts =
        fingerprint::FunctionStarts({ std::istreambuf_iterator<char>(list), std::istreambuf_iterator<char>() });
    std::ifstream header(argv[3]);
    const std::string text{ std::istreambuf_iterator<char>(header), std::istreambuf_iterator<char>() };
    auto addresses = fingerprint::ReadAddresses(text, code.base, uint32_t(code.words.size() * 4));
    std::vector<std::string> missing;
    auto prints = fingerprint::Make(code, starts, addresses, missing);

    // Each is found again at its own address in the same image (its fingerprint tells it apart).
    unsigned again = 0;
    for (const auto& found : fingerprint::Find(code, prints))
    {
        if (found.theirs == found.ours)
            again++;
        else
            fprintf(stderr, "warning: %s (%08X) is %s in its own image\n", found.name.c_str(), found.ours,
                found.theirs ? "found elsewhere" : found.confidence.c_str());
    }
    std::ofstream(argv[4], std::ios::binary) << fingerprint::Write(prints);
    printf("%zu addresses: %zu fingerprints, %u found again at their place; none for:", addresses.size(), prints.size(),
        again);
    for (const auto& name : missing)
        printf(" %s", name.c_str());
    printf("\n");
    return 0;
}
