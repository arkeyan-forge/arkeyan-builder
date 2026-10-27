// SHA-256 (FIPS 180-4) of a file, as lowercase hex; empty when the file cannot be read.
#pragma once

#include <filesystem>
#include <string>

std::string Sha256File(const std::filesystem::path& path);
std::string Sha256Text(const std::string& text);
