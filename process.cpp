#include "process.h"
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif
extern char** environ;
#endif

#ifdef _WIN32
static std::wstring Widen(const std::string& text)
{
    if (text.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
    return wide;
}

static std::string Narrow(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string text(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// The quoting CommandLineToArgvW and the C runtimes undo.
static void AppendQuoted(std::wstring& line, const std::wstring& arg)
{
    if (!line.empty())
        line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        line += arg;
        return;
    }
    line += L'"';
    for (size_t i = 0;; i++)
    {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\')
        {
            i++;
            backslashes++;
        }
        if (i == arg.size())
        {
            line.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
            line.append(backslashes * 2 + 1, L'\\');
        else
            line.append(backslashes, L'\\');
        line += arg[i];
    }
    line += L'"';
}

int Run(const std::vector<std::string>& args, const RunOptions& options)
{
    std::wstring commandLine;
    for (const std::string& arg : args)
        AppendQuoted(commandLine, Widen(arg));

    SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
    HANDLE output = CreateFileW(options.log.empty() ? L"NUL" : options.log.wstring().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

    // Only these two handles are inherited: children started at the same time by other threads must not keep this
    // child's log open (it could not be deleted afterwards).
    HANDLE inherited[2] = { input, output };
    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<char> attributes(attributesSize);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    InitializeProcThreadAttributeList(attributeList, 1, 0, &attributesSize);
    UpdateProcThreadAttribute(
        attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = output;
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    std::wstring cwd = options.cwd.empty() ? std::wstring() : options.cwd.wstring();
    BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
        &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(output);
    CloseHandle(input);
    if (!started)
        return -1;
    CloseHandle(process.hThread);

    int result = 0;
    auto start = std::chrono::steady_clock::now();
    while (true)
    {
        if (WaitForSingleObject(process.hProcess, 500) == WAIT_OBJECT_0)
        {
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            result = int(code);
            break;
        }
        bool timedOut = options.timeoutSeconds > 0 &&
                        std::chrono::steady_clock::now() - start > std::chrono::seconds(options.timeoutSeconds);
        bool stop = !timedOut && options.poll && options.poll();
        if (timedOut || stop)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            result = timedOut ? -2 : -3;
            break;
        }
    }
    CloseHandle(process.hProcess);
    return result;
}

void SetEnv(const char* name, const std::string& value)
{
    SetEnvironmentVariableW(Widen(name).c_str(), value.empty() ? nullptr : Widen(value).c_str());
}

std::string GetEnv(const char* name)
{
    wchar_t buffer[32768];
    DWORD size = GetEnvironmentVariableW(Widen(name).c_str(), buffer, 32768);
    return size > 0 && size < 32768 ? Narrow(std::wstring(buffer, size)) : std::string();
}

uint64_t PhysicalMemoryBytes()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
}

fs::path ExecutableDirectory()
{
    wchar_t buffer[32768];
    DWORD size = GetModuleFileNameW(nullptr, buffer, 32768);
    return fs::path(std::wstring(buffer, size)).parent_path();
}

std::string ShortPath(const fs::path& path)
{
    wchar_t buffer[32768];
    DWORD size = GetShortPathNameW(path.wstring().c_str(), buffer, 32768);
    return size > 0 && size < 32768 ? Narrow(std::wstring(buffer, size)) : Narrow(path.wstring());
}

fs::path PathFromUtf8(const std::string& text) { return fs::path(Widen(text)); }

std::string Slashes(const fs::path& path)
{
    std::string text = Narrow(path.wstring());
    for (char& c : text)
        if (c == '\\')
            c = '/';
    return text;
}
#else
int Run(const std::vector<std::string>& args, const RunOptions& options)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    std::string log = options.log.empty() ? "/dev/null" : options.log.string();
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    if (!options.cwd.empty())
    {
        // macOS 26 replaced the _np call with the POSIX one; older systems (the kit's minimum) only have the _np one.
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
        posix_spawn_file_actions_addchdir(&actions, options.cwd.c_str());
#else
        posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str());
#endif
    }
    std::vector<char*> argv;
    for (const std::string& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0)
        return -1;

    auto start = std::chrono::steady_clock::now();
    int ticks = 0;
    while (true)
    {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (done < 0)
            return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (++ticks % 25 != 0)
            continue;
        bool timedOut = options.timeoutSeconds > 0 &&
                        std::chrono::steady_clock::now() - start > std::chrono::seconds(options.timeoutSeconds);
        bool stop = !timedOut && options.poll && options.poll();
        if (timedOut || stop)
        {
            kill(pid, SIGKILL); // our own child only
            waitpid(pid, &status, 0);
            return timedOut ? -2 : -3;
        }
    }
}

void SetEnv(const char* name, const std::string& value)
{
    if (value.empty())
        unsetenv(name);
    else
        setenv(name, value.c_str(), 1);
}

std::string GetEnv(const char* name)
{
    const char* value = getenv(name);
    return value != nullptr ? value : "";
}

uint64_t PhysicalMemoryBytes()
{
#ifdef __APPLE__
    uint64_t memory = 0;
    size_t size = sizeof(memory);
    return sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0 ? memory : 0;
#else
    return uint64_t(sysconf(_SC_PHYS_PAGES)) * uint64_t(sysconf(_SC_PAGE_SIZE));
#endif
}

fs::path ExecutableDirectory()
{
#ifdef __APPLE__
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0)
        return fs::weakly_canonical(buffer).parent_path();
    return fs::current_path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
}

std::string ShortPath(const fs::path& path) { return path.string(); }

fs::path PathFromUtf8(const std::string& text) { return fs::path(text); }

std::string Slashes(const fs::path& path) { return path.string(); }
#endif
