// See shader_cache_literals.h.
#include "shader_cache_literals.h"
#include <cstdint>

std::string ShaderCacheLiterals(const std::string& text)
{
    static const std::string Open = "Cache[] = {";
    std::string out;
    out.reserve(text.size());
    size_t at = 0;
    for (size_t found; (found = text.find(Open, at)) != std::string::npos;)
    {
        size_t begin = found + Open.size(), end = text.find('}', begin);
        if (end == std::string::npos || text.find_first_not_of("0123456789,", begin) != end)
            break; // not a list of numbers: the rest is kept as it is
        out.append(text, at, begin - 1 - at); // up to "= "
        out += "\n\"";
        size_t column = 0;
        unsigned value = 0;
        bool digits = false;
        auto put = [&]() {
            uint8_t byte = uint8_t(value);
            // Printable ASCII as itself, but '"', '\' and '?' (trigraphs); the rest as 3-digit octal, which no digit
            // after it can extend.
            if (byte >= 0x20 && byte < 0x7F && byte != '"' && byte != '\\' && byte != '?')
                out += char(byte);
            else
            {
                char escape[5] = { '\\', char('0' + (byte >> 6)), char('0' + ((byte >> 3) & 7)),
                    char('0' + (byte & 7)) };
                out.append(escape, 4);
            }
            if (++column == 4096) // short lines: the literal is the concatenation of all of them
            {
                out += "\"\n\"";
                column = 0;
            }
            value = 0;
            digits = false;
        };
        for (size_t i = begin; i < end; i++)
        {
            if (text[i] == ',')
            {
                if (digits)
                    put();
            }
            else
            {
                value = value * 10 + unsigned(text[i] - '0');
                digits = true;
            }
        }
        if (digits)
            put();
        out += "\"";
        at = end + 1;
    }
    out.append(text, at, std::string::npos);
    return out;
}
