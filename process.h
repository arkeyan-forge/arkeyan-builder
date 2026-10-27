// Child processes and small host queries for SkyBuild: no shell is involved, arguments are passed as they are.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct RunOptions
{
    fs::path cwd;                 // empty: the current directory
    fs::path log;                 // stdout and stderr of the child; empty: discarded
    int timeoutSeconds = 0;       // 0: none; the child is then killed and Run returns -2
    // Called about twice a second while the child runs; returning true kills it (Run then returns -3).
    std::function<bool()> poll;
};

// Exit code, or -1 when the program could not be started.
int Run(const std::vector<std::string>& args, const RunOptions& options = {});

// Sets (or, with an empty value, removes) a variable of this process; children inherit it.
void SetEnv(const char* name, const std::string& value);
std::string GetEnv(const char* name);

uint64_t PhysicalMemoryBytes();
fs::path ExecutableDirectory();
// UTF-8 path as the OS hands it to programs; on Windows, the 8.3 form when there is one (ASCII), for children
// whose own code page may not be UTF-8.
std::string ShortPath(const fs::path& path);
fs::path PathFromUtf8(const std::string& text);
// Forward slashes on every OS, UTF-8.
std::string Slashes(const fs::path& path);
