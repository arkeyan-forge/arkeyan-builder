#include "toolchain_fetch.h"
#include "process.h"
#include <miniz.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
namespace
{
    std::wstring Widen(const std::string& text)
    {
        int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
        std::wstring wide(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
        return wide;
    }

    struct Handle
    {
        HINTERNET value;
        explicit Handle(HINTERNET h) : value(h) {}
        ~Handle()
        {
            if (value)
                WinHttpCloseHandle(value);
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    bool DownloadOnce(const std::string& url, const fs::path& file, const std::function<void(uint64_t)>& progress,
        std::string& error)
    {
        std::wstring wideUrl = Widen(url);
        URL_COMPONENTSW parts = {};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(wideUrl.c_str(), DWORD(wideUrl.size()), 0, &parts))
        {
            error = "bad address " + url;
            return false;
        }
        // The system proxy settings, as a browser has them; Windows 8.1 and later.
        Handle session(WinHttpOpen(L"Arkeyan-Builder", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
        if (!session.value)
            session.value = WinHttpOpen(L"Arkeyan-Builder", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        if (!session.value)
        {
            error = "WinHTTP error " + std::to_string(GetLastError());
            return false;
        }
        WinHttpSetTimeouts(session.value, 30000, 30000, 60000, 60000);
        Handle connection(WinHttpConnect(
            session.value, std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(), parts.nPort, 0));
        if (!connection.value)
        {
            error = "could not connect (WinHTTP error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);
        Handle request(WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        if (!request.value || !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.value, nullptr))
        {
            error = "no answer (WinHTTP error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        DWORD status = 0;
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200)
        {
            error = "the server answered " + std::to_string(status);
            return false;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "could not write " + Slashes(file);
            return false;
        }
        std::vector<char> buffer(256 * 1024);
        uint64_t total = 0;
        for (;;)
        {
            DWORD got = 0;
            if (!WinHttpReadData(request.value, buffer.data(), DWORD(buffer.size()), &got))
            {
                error = "the connection broke (WinHTTP error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            if (got == 0)
                break;
            out.write(buffer.data(), got);
            total += got;
            progress(total);
        }
        out.close();
        if (!out)
        {
            error = "could not write " + Slashes(file);
            return false;
        }
        return true;
    }
}

bool HttpDownload(const std::string& url, const fs::path& file, const std::function<void(uint64_t)>& progress,
    std::string& error)
{
    for (int attempt = 1;; ++attempt)
    {
        if (DownloadOnce(url, file, progress, error))
            return true;
        if (attempt == 4)
            return false;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
#else
bool HttpDownload(const std::string&, const fs::path&, const std::function<void(uint64_t)>&, std::string& error)
{
    error = "this build has no in-process download";
    return false;
}
#endif

namespace
{
    uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
    uint32_t U32(const uint8_t* p) { return uint32_t(U16(p)) | uint32_t(U16(p + 2)) << 16; }

    struct Entry
    {
        std::string name;
        uint16_t method;
        uint32_t crc, packed, size, header;
    };

    // The entry's path below the extraction folder, empty when it would leave it.
    fs::path SafePath(std::string name)
    {
        for (char& c : name)
            if (c == '\\')
                c = '/';
        fs::path result;
        size_t start = 0;
        while (start <= name.size())
        {
            size_t end = name.find('/', start);
            if (end == std::string::npos)
                end = name.size();
            std::string part = name.substr(start, end - start);
            if (part == ".." || part.find(':') != std::string::npos || (start == 0 && end == 0 && !name.empty()))
                return {};
            if (!part.empty() && part != ".")
                result /= PathFromUtf8(part);
            start = end + 1;
        }
        return result;
    }

    bool Inflate(std::ifstream& in, const Entry& entry, std::ofstream& out, uint32_t& crc, std::string& error)
    {
        constexpr size_t Dictionary = TINFL_LZ_DICT_SIZE;
        std::vector<uint8_t> window(Dictionary), input(64 * 1024);
        tinfl_decompressor state;
        tinfl_init(&state);
        size_t windowOffset = 0, available = 0, consumed = 0;
        uint32_t left = entry.packed;
        for (;;)
        {
            if (consumed == available && left > 0)
            {
                available = std::min<size_t>(input.size(), left);
                in.read(reinterpret_cast<char*>(input.data()), std::streamsize(available));
                if (size_t(in.gcount()) != available)
                {
                    error = "the archive is truncated";
                    return false;
                }
                left -= uint32_t(available);
                consumed = 0;
            }
            size_t inBytes = available - consumed, outBytes = Dictionary - windowOffset;
            tinfl_status status = tinfl_decompress(&state, input.data() + consumed, &inBytes, window.data(),
                window.data() + windowOffset, &outBytes, left > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            consumed += inBytes;
            out.write(reinterpret_cast<const char*>(window.data() + windowOffset), std::streamsize(outBytes));
            crc = uint32_t(mz_crc32(crc, window.data() + windowOffset, outBytes));
            windowOffset = (windowOffset + outBytes) & (Dictionary - 1);
            if (status == TINFL_STATUS_DONE)
                return true;
            if (status < 0 || (status == TINFL_STATUS_NEEDS_MORE_INPUT && left == 0 && consumed == available))
            {
                error = "bad compressed data in " + entry.name;
                return false;
            }
        }
    }
}

bool ExtractZip(const fs::path& archive, const fs::path& folder, const std::function<void(uint64_t, uint64_t)>& progress,
    std::string& error)
{
    std::ifstream in(archive, std::ios::binary);
    if (!in)
    {
        error = "could not open " + Slashes(archive);
        return false;
    }
    in.seekg(0, std::ios::end);
    uint64_t length = uint64_t(in.tellg());
    // The end-of-central-directory record is in the last 64 KB plus its own 22 bytes.
    size_t tail = size_t(std::min<uint64_t>(length, 22 + 65535));
    std::vector<uint8_t> end(tail);
    in.seekg(std::streamoff(length - tail));
    in.read(reinterpret_cast<char*>(end.data()), std::streamsize(tail));
    size_t at = tail;
    while (at-- > 0 && !(at + 22 <= tail && U32(&end[at]) == 0x06054b50))
        ;
    if (at == size_t(-1))
    {
        error = "not a zip archive";
        return false;
    }
    uint32_t count = U16(&end[at + 10]), directorySize = U32(&end[at + 12]), directoryOffset = U32(&end[at + 16]);
    if (count == 0xFFFF || directorySize == 0xFFFFFFFF || directoryOffset == 0xFFFFFFFF)
    {
        error = "Zip64 archives are not supported";
        return false;
    }
    if (uint64_t(directoryOffset) + directorySize > length)
    {
        error = "the archive is truncated";
        return false;
    }
    std::vector<uint8_t> directory(directorySize);
    in.seekg(directoryOffset);
    in.read(reinterpret_cast<char*>(directory.data()), std::streamsize(directorySize));

    std::vector<Entry> entries;
    uint64_t total = 0;
    size_t p = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (p + 46 > directory.size() || U32(&directory[p]) != 0x02014b50)
        {
            error = "the archive's file list is damaged";
            return false;
        }
        const uint8_t* h = &directory[p];
        size_t nameLength = U16(h + 28), skip = nameLength + U16(h + 30) + U16(h + 32);
        if (p + 46 + skip > directory.size())
        {
            error = "the archive's file list is damaged";
            return false;
        }
        Entry entry{ std::string(reinterpret_cast<const char*>(h + 46), nameLength), U16(h + 10), U32(h + 16),
            U32(h + 20), U32(h + 24), U32(h + 42) };
        if (U16(h + 8) & 1)
        {
            error = "an encrypted entry: " + entry.name;
            return false;
        }
        // Made on Unix (high byte 3): the mode is the high half of the external attributes.
        if ((U16(h + 4) >> 8) == 3 && ((U32(h + 38) >> 16) & 0170000) == 0120000)
        {
            error = "a symbolic link: " + entry.name;
            return false;
        }
        if (entry.packed == 0xFFFFFFFF || entry.size == 0xFFFFFFFF || entry.header == 0xFFFFFFFF)
        {
            error = "Zip64 archives are not supported";
            return false;
        }
        total += entry.size;
        entries.push_back(std::move(entry));
        p += 46 + skip;
    }

    uint64_t done = 0;
    std::error_code ignored;
    for (const Entry& entry : entries)
    {
        fs::path relative = SafePath(entry.name);
        if (relative.empty())
        {
            if (entry.name.empty() || entry.name == "./")
                continue;
            error = "an entry outside the folder: " + entry.name;
            return false;
        }
        fs::path target = folder / relative;
        if (entry.name.back() == '/' || entry.name.back() == '\\')
        {
            fs::create_directories(target, ignored);
            continue;
        }
        fs::create_directories(target.parent_path(), ignored);
        uint8_t local[30];
        in.seekg(entry.header);
        in.read(reinterpret_cast<char*>(local), sizeof(local));
        if (!in || U32(local) != 0x04034b50)
        {
            error = "the archive is damaged at " + entry.name;
            return false;
        }
        in.seekg(std::streamoff(entry.header) + 30 + U16(local + 26) + U16(local + 28));
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "could not write " + Slashes(target);
            return false;
        }
        uint32_t crc = uint32_t(mz_crc32(0, nullptr, 0));
        if (entry.method == 8)
        {
            if (!Inflate(in, entry, out, crc, error))
                return false;
        }
        else if (entry.method == 0)
        {
            std::vector<char> block(64 * 1024);
            for (uint32_t left = entry.packed; left > 0;)
            {
                size_t n = std::min<size_t>(block.size(), left);
                in.read(block.data(), std::streamsize(n));
                if (size_t(in.gcount()) != n)
                {
                    error = "the archive is truncated";
                    return false;
                }
                out.write(block.data(), std::streamsize(n));
                crc = uint32_t(mz_crc32(crc, reinterpret_cast<const uint8_t*>(block.data()), n));
                left -= uint32_t(n);
            }
        }
        else
        {
            error = "an unsupported compression method in " + entry.name;
            return false;
        }
        out.close();
        if (!out || crc != entry.crc || fs::file_size(target, ignored) != entry.size)
        {
            error = "a bad file in the archive: " + entry.name;
            return false;
        }
        done += entry.size;
        progress(done, total);
    }
    return true;
}
