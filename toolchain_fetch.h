// The Windows compiler fetch without curl.exe and tar.exe: an HTTPS download (WinHTTP) and a .zip extraction (miniz's
// inflate), both inside Arkeyan-Builder. Errors come back as text, never as an exception.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Downloads `url` to `file`, following redirects, three attempts 5 seconds apart. `progress` gets the bytes received.
// Windows only (WinHTTP); elsewhere it fails at once.
bool HttpDownload(const std::string& url, const std::filesystem::path& file,
    const std::function<void(uint64_t)>& progress, std::string& error);

// Extracts every file of `archive` below `folder`. Refuses an entry that would land outside it, an encrypted or a
// symbolic-link entry, a Zip64 archive, and a file whose CRC-32 or size is wrong. `progress` gets (bytes, total).
bool ExtractZip(const std::filesystem::path& archive, const std::filesystem::path& folder,
    const std::function<void(uint64_t, uint64_t)>& progress, std::string& error);
