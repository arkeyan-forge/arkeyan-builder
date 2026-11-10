#include "fingerprint.h"
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace skybuild::fingerprint
{
    namespace
    {
        constexpr uint64_t FnvBasis = 0xCBF29CE484222325ull, FnvPrime = 0x100000001B3ull;

        uint64_t Mix(uint64_t hash, uint32_t word)
        {
            for (int i = 0; i < 4; i++)
                hash = (hash ^ ((word >> (24 - 8 * i)) & 0xFF)) * FnvPrime;
            return hash;
        }

        bool IsLis(uint32_t word) { return word >> 26 == 15 && ((word >> 16) & 31) == 0; }

        // The instructions that take the low half of an address loaded by lis: addi, ori, the D-form loads and stores,
        // ld and std. The register holding the high half: rS for ori, rA for the others.
        std::optional<unsigned> LowHalfSource(uint32_t word)
        {
            unsigned op = word >> 26;
            if (op == 24)
                return (word >> 21) & 31;
            if (op == 14 || (op >= 32 && op <= 55) || op == 58 || op == 62)
                return (word >> 16) & 31;
            return std::nullopt;
        }

        // The word with the fields a link sets masked; `loaded`: the registers a lis of this function set so far.
        uint32_t Masked(uint32_t word, uint32_t& loaded)
        {
            if (word >> 26 == 18) // b, bl: the target
                return word & 0xFC000003;
            if (IsLis(word))
            {
                loaded |= 1u << ((word >> 21) & 31);
                return word & 0xFFFF0000;
            }
            if (auto source = LowHalfSource(word); source && *source != 0 && (loaded >> *source & 1))
                return word & 0xFFFF0000;
            return word;
        }

        // The address a lis at `hi` and the low half at `lo` make.
        uint32_t PairAddress(uint32_t hi, uint32_t lo)
        {
            uint32_t high = (hi & 0xFFFF) << 16;
            return lo >> 26 == 24 ? (high | (lo & 0xFFFF)) : high + uint32_t(int32_t(int16_t(lo & 0xFFFF)));
        }

        std::optional<uint32_t> CallTarget(uint32_t address, uint32_t word)
        {
            if (word >> 26 != 18 || (word & 3) != 1) // bl, relative
                return std::nullopt;
            uint32_t offset = word & 0x03FFFFFC;
            return address + (offset & 0x02000000 ? offset - 0x04000000 : offset);
        }

        std::string Hex(uint64_t value, int digits)
        {
            char text[24];
            snprintf(text, sizeof(text), "%0*llX", digits, static_cast<unsigned long long>(value));
            return text;
        }
    }

    bool Code::IsCode(uint32_t address, uint32_t count) const
    {
        for (const auto& [start, end] : sections)
            if (address >= start && address < end && (end - address) / 4 >= count)
                return true;
        return false;
    }

    Code FromBytes(uint32_t base, const uint8_t* data, size_t size, std::vector<std::pair<uint32_t, uint32_t>> sections)
    {
        Code code;
        code.base = base;
        code.words.resize(size / 4);
        for (size_t i = 0; i < code.words.size(); i++)
            code.words[i] = uint32_t(data[i * 4]) << 24 | uint32_t(data[i * 4 + 1]) << 16 |
                            uint32_t(data[i * 4 + 2]) << 8 | data[i * 4 + 3];
        for (auto& [start, end] : sections)
        {
            start = std::max(start, base);
            end = std::min<uint64_t>(end, uint64_t(base) + code.words.size() * 4);
            if (start < end)
                code.sections.emplace_back(start & ~3u, end & ~3u);
        }
        return code;
    }

    uint64_t Hash(const Code& code, uint32_t start, uint32_t words)
    {
        uint64_t hash = FnvBasis;
        uint32_t loaded = 0;
        for (uint32_t i = 0; i < words; i++)
            hash = Mix(hash, Masked(code.Word(start + i * 4), loaded));
        return hash;
    }

    uint64_t Head(const Code& code, uint32_t start)
    {
        // Every low-half immediate masked, lis'd or not: a word equal under Hash's masks is equal here.
        uint64_t hash = FnvBasis;
        for (uint32_t i = 0; i < 4; i++)
        {
            uint32_t loaded = 0, word = Masked(code.Word(start + i * 4), loaded);
            if (LowHalfSource(word))
                word &= 0xFFFF0000;
            hash = Mix(hash, word);
        }
        return hash;
    }

    std::vector<Callee> Callees(
        const Code& code, uint32_t start, uint32_t words, const std::map<uint32_t, uint32_t>& lengths)
    {
        std::vector<Callee> callees;
        for (uint32_t i = 0; i < words; i++)
        {
            uint32_t address = start + i * 4;
            if (auto target = CallTarget(address, code.Word(address)))
            {
                auto length = lengths.find(*target);
                if (length != lengths.end() && code.IsCode(*target, length->second))
                    callees.push_back({ length->second, Hash(code, *target, length->second) });
                else
                    callees.push_back({}); // an import's thunk, or no function there: not compared
            }
        }
        return callees;
    }

    std::vector<uint32_t> FunctionStarts(const std::string& file)
    {
        std::vector<uint32_t> starts;
        auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); };
        if (file.rfind("!<arch>", 0) == 0)
        {
            for (size_t at = file.find("sub_"); at != std::string::npos; at = file.find("sub_", at + 4))
            {
                if (at + 12 > file.size() || !std::all_of(file.begin() + at + 4, file.begin() + at + 12, hex) ||
                    (at + 12 < file.size() && hex(file[at + 12])))
                    continue;
                starts.push_back(uint32_t(std::stoul(file.substr(at + 4, 8), nullptr, 16)));
            }
        }
        else
        {
            std::istringstream lines(file);
            for (std::string line; std::getline(lines, line);)
                if (line.size() >= 8 && std::all_of(line.begin(), line.begin() + 8, hex))
                    starts.push_back(uint32_t(std::stoul(line.substr(0, 8), nullptr, 16)));
        }
        std::sort(starts.begin(), starts.end());
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
        return starts;
    }

    std::vector<std::pair<std::string, uint32_t>> ReadAddresses(const std::string& header, uint32_t base, uint32_t size)
    {
        // Continued lines joined, comments dropped.
        std::string text;
        std::istringstream lines(header);
        for (std::string line; std::getline(lines, line);)
        {
            if (size_t comment = line.find("//"); comment != std::string::npos)
                line.erase(comment);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
                line.pop_back();
            bool continued = !line.empty() && line.back() == '\\';
            if (continued)
                line.pop_back();
            text += line + (continued ? " " : "\n");
        }
        std::vector<std::pair<std::string, uint32_t>> addresses;
        static const std::regex hook(R"(#define\s+(GAME_HOOK_\w+)\s+sub_([0-9A-Fa-f]{8})\b)");
        static const std::regex value(R"((\w+)\s*=\s*0x([0-9A-Fa-f]{8})\b)");
        std::istringstream joined(text);
        for (std::string line; std::getline(joined, line);)
        {
            std::smatch match;
            if (std::regex_search(line, match, hook))
                addresses.emplace_back(match[1], uint32_t(std::stoul(match[2], nullptr, 16)));
            else if (line.find("constexpr") != std::string::npos)
                for (std::sregex_iterator it(line.begin(), line.end(), value), end; it != end; ++it)
                {
                    uint32_t address = uint32_t(std::stoul((*it)[2], nullptr, 16));
                    if (address >= base && address - base < size)
                        addresses.emplace_back((*it)[1], address);
                }
        }
        return addresses;
    }

    std::vector<Print> Make(const Code& code, const std::vector<uint32_t>& starts,
        const std::vector<std::pair<std::string, uint32_t>>& addresses, std::vector<std::string>& missing)
    {
        std::map<uint32_t, uint32_t> lengths; // start -> words
        for (size_t i = 0; i + 1 < starts.size(); i++)
        {
            // Up to the next start, less the zero words of alignment (no instruction is zero): another build may pad
            // the same function otherwise.
            uint32_t words = starts[i + 1] > starts[i] ? (starts[i + 1] - starts[i]) / 4 : 0;
            if (words == 0 || !code.IsCode(starts[i], words))
                continue;
            while (words > 1 && code.Word(starts[i] + (words - 1) * 4) == 0)
                words--;
            lengths[starts[i]] = words;
        }
        std::unordered_map<uint64_t, unsigned> uses; // how many functions have each hash
        std::map<uint32_t, uint64_t> hashes;
        for (const auto& [start, words] : lengths)
            uses[hashes[start] = Hash(code, start, words)]++;

        auto function = [&](const std::string& name, uint32_t address, uint32_t start) {
            Print print;
            print.name = name;
            print.address = address;
            print.words = lengths.at(start);
            print.hash = hashes.at(start);
            print.head = Head(code, start);
            print.callees = Callees(code, start, print.words, lengths);
            return print;
        };
        std::vector<Print> prints;
        for (const auto& [name, address] : addresses)
        {
            if (lengths.count(address))
            {
                prints.push_back(function(name, address, address));
                continue;
            }
            // Inside a function: that function, and how far in.
            if (auto next = lengths.upper_bound(address); next != lengths.begin())
            {
                const auto [start, words] = *std::prev(next);
                if (address < start + words * 4 && uses[hashes[start]] == 1)
                {
                    Print print = function(name, address, start);
                    print.inside = true;
                    print.hi = (address - start) / 4;
                    prints.push_back(print);
                    continue;
                }
            }
            // Data: the first function, of a hash no other function has, that builds this address with a lis pair.
            bool found = false;
            for (auto it = lengths.begin(); it != lengths.end() && !found; ++it)
            {
                const auto [start, words] = *it;
                if (uses[hashes[start]] != 1 || code.IsCode(address))
                    continue;
                std::map<unsigned, uint32_t> his; // register -> index of its lis
                for (uint32_t i = 0; i < words && !found; i++)
                {
                    uint32_t word = code.Word(start + i * 4);
                    if (IsLis(word))
                        his[(word >> 21) & 31] = i;
                    else if (auto source = LowHalfSource(word);
                        source && his.count(*source) &&
                        PairAddress(code.Word(start + his[*source] * 4), word) == address)
                    {
                        Print print = function(name, address, start);
                        print.data = true;
                        print.hi = his[*source];
                        print.lo = i;
                        prints.push_back(print);
                        found = true;
                    }
                }
            }
            if (!found)
                missing.push_back(name);
        }
        return prints;
    }

    std::string Write(const std::vector<Print>& prints)
    {
        // kind: fn (a function's start), in (`hi` words into the function), data (built by the pair at `hi`, `lo`).
        std::string text = "# <kind> <name> <address> <words> <hash> <head> <hi> <lo> <callees: words:hash,...>\n";
        for (const Print& print : prints)
        {
            text += std::string(print.data     ? "data "
                                : print.inside ? "in "
                                               : "fn ") +
                    print.name + " " + Hex(print.address, 8) + " " + std::to_string(print.words) + " " +
                    Hex(print.hash, 16) + " " + Hex(print.head, 16) + " " + std::to_string(print.hi) + " " +
                    std::to_string(print.lo) + " ";
            std::string callees;
            for (const Callee& callee : print.callees)
                callees += (callees.empty() ? "" : ",") + std::to_string(callee.words) + ":" + Hex(callee.hash, 16);
            text += (callees.empty() ? "-" : callees) + "\n";
        }
        return text;
    }

    std::vector<Print> Read(const std::string& text)
    {
        std::vector<Print> prints;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);)
        {
            std::istringstream fields(line);
            std::string kind, address, hash, head, callees;
            Print print;
            if (!(fields >> kind >> print.name >> address >> print.words >> hash >> head >> print.hi >> print.lo >>
                    callees) ||
                (kind != "fn" && kind != "data" && kind != "in") || print.words == 0)
                continue;
            print.data = kind == "data";
            print.inside = kind == "in";
            print.address = uint32_t(std::stoul(address, nullptr, 16));
            print.hash = std::stoull(hash, nullptr, 16);
            print.head = std::stoull(head, nullptr, 16);
            std::istringstream list(callees == "-" ? "" : callees);
            for (std::string item; std::getline(list, item, ',');)
            {
                size_t colon = item.find(':');
                if (colon != std::string::npos)
                    print.callees.push_back({ uint32_t(std::stoul(item.substr(0, colon))),
                        std::stoull(item.substr(colon + 1), nullptr, 16) });
            }
            if (print.hi < print.words && print.lo < print.words)
                prints.push_back(std::move(print));
        }
        return prints;
    }

    std::vector<Found> Find(const Code& code, const std::vector<Print>& prints)
    {
        // Every code position's head, once.
        std::vector<std::pair<uint32_t, uint64_t>> heads;
        for (const auto& [start, end] : code.sections)
            for (uint32_t address = start; address + 16 <= end; address += 4)
                heads.emplace_back(address, Head(code, address));

        std::vector<Found> found;
        for (const Print& print : prints)
        {
            std::vector<std::pair<uint32_t, unsigned>> matches; // start, callees that match
            unsigned known = 0;
            for (const Callee& callee : print.callees)
                known += callee.words != 0;
            for (const auto& [start, head] : heads)
            {
                if (head != print.head || !code.IsCode(start, print.words) ||
                    Hash(code, start, print.words) != print.hash)
                    continue;
                unsigned score = 0;
                size_t call = 0;
                for (uint32_t i = 0; i < print.words && call < print.callees.size(); i++)
                {
                    uint32_t address = start + i * 4;
                    auto target = CallTarget(address, code.Word(address));
                    if (!target)
                        continue;
                    const Callee& callee = print.callees[call++];
                    score += callee.words != 0 && code.IsCode(*target, callee.words) &&
                             Hash(code, *target, callee.words) == callee.hash;
                }
                matches.emplace_back(start, score);
            }
            Found result{ print.name, print.address, 0, "not found" };
            uint32_t start = 0;
            if (matches.size() == 1)
            {
                start = matches[0].first;
                result.confidence = "sure";
            }
            else if (matches.size() > 1)
            {
                std::sort(matches.begin(), matches.end(), [](auto& a, auto& b) { return a.second > b.second; });
                if (matches[0].second > matches[1].second)
                {
                    start = matches[0].first;
                    result.confidence = known != 0 && matches[0].second == known ? "call graph" : "likely";
                }
                else
                    result.confidence = "ambiguous: " + std::to_string(matches.size()) + " candidates";
            }
            if (start != 0)
                result.theirs = print.data
                                    ? PairAddress(code.Word(start + print.hi * 4), code.Word(start + print.lo * 4))
                                : print.inside ? start + print.hi * 4
                                               : start;
            found.push_back(result);
        }
        return found;
    }
}
