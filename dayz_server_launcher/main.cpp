#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    using BytePattern = std::vector<int>;

    struct Patch
    {
        const char* name;
        BytePattern pattern;
        std::vector<BYTE> replacement;
    };

    std::wstring lowercase(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return value;
    }

    fs::path launcherPath()
    {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (!length || length == buffer.size())
            throw std::runtime_error("cannot determine launcher path");
        buffer.resize(length);
        return fs::path(buffer);
    }

    fs::path defaultTarget(const fs::path& self, std::wstring_view filename)
    {
        const fs::path sibling = self.parent_path() / filename;
        if (fs::is_regular_file(sibling)) return sibling;
        return fs::current_path() / filename;
    }

    std::wstring quoteArgument(std::wstring_view argument)
    {
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") ==
            std::wstring_view::npos)
            return std::wstring(argument);

        std::wstring result(1, L'\"');
        std::size_t backslashes = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::wstring buildCommandLine(const fs::path& executable,
        const std::vector<std::wstring>& arguments)
    {
        std::wstring result = quoteArgument(executable.wstring());
        for (const auto& argument : arguments)
        {
            result.push_back(L' ');
            result += quoteArgument(argument);
        }
        return result;
    }

    std::string windowsError(DWORD code)
    {
        wchar_t* message{};
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
            0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
        std::wstring wide = length && message ? std::wstring(message, length) :
            L"unknown Windows error";
        if (message) LocalFree(message);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n'))
            wide.pop_back();
        const int required = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(required, '\0');
        if (required)
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                utf8.data(), required, nullptr, nullptr);
        return utf8;
    }

    std::vector<BYTE> readFile(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input)
            throw std::runtime_error("cannot open target executable for patch scanning");
        const auto length = input.tellg();
        if (length <= 0 || static_cast<std::uintmax_t>(length) >
            std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("target executable has an invalid size");
        std::vector<BYTE> bytes(static_cast<std::size_t>(length));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), length))
            throw std::runtime_error("cannot read target executable for patch scanning");
        return bytes;
    }

    template<typename T>
    T structureAt(const std::vector<BYTE>& bytes, std::size_t offset,
        const char* description)
    {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
            throw std::runtime_error(std::string("invalid PE: truncated ") + description);
        T result{};
        std::memcpy(&result, bytes.data() + offset, sizeof(result));
        return result;
    }

    std::size_t findUniquePattern(const std::vector<BYTE>& bytes, const Patch& patch)
    {
        if (patch.pattern.empty() || patch.pattern.size() > bytes.size())
            throw std::runtime_error(std::string("patch signature not found: ") + patch.name);

        std::size_t result = std::numeric_limits<std::size_t>::max();
        for (std::size_t offset = 0; offset <= bytes.size() - patch.pattern.size(); ++offset)
        {
            bool match = true;
            for (std::size_t index = 0; index < patch.pattern.size(); ++index)
            {
                const int expected = patch.pattern[index];
                if (expected >= 0 && bytes[offset + index] != expected)
                {
                    match = false;
                    break;
                }
            }
            if (!match) continue;
            if (result != std::numeric_limits<std::size_t>::max())
                throw std::runtime_error(std::string("patch signature is not unique: ") +
                    patch.name);
            result = offset;
        }
        if (result == std::numeric_limits<std::size_t>::max())
            throw std::runtime_error(std::string("patch signature not found: ") + patch.name);
        return result;
    }

    DWORD fileOffsetToRva(const std::vector<BYTE>& bytes, std::size_t fileOffset)
    {
        const auto dos = structureAt<IMAGE_DOS_HEADER>(bytes, 0, "DOS header");
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
            throw std::runtime_error("invalid PE: bad DOS header");
        const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
        const auto nt = structureAt<IMAGE_NT_HEADERS64>(bytes, ntOffset, "NT headers");
        if (nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
            throw std::runtime_error("target executable is not a valid x64 PE image");

        if (fileOffset < nt.OptionalHeader.SizeOfHeaders)
            return static_cast<DWORD>(fileOffset);

        const std::size_t sectionTable = ntOffset + offsetof(IMAGE_NT_HEADERS64,
            OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        for (WORD index = 0; index < nt.FileHeader.NumberOfSections; ++index)
        {
            const auto section = structureAt<IMAGE_SECTION_HEADER>(bytes,
                sectionTable + static_cast<std::size_t>(index) * sizeof(IMAGE_SECTION_HEADER),
                "section table");
            const std::size_t rawStart = section.PointerToRawData;
            const std::size_t rawSize = section.SizeOfRawData;
            if (fileOffset >= rawStart && fileOffset - rawStart < rawSize)
            {
                const std::uint64_t rva = static_cast<std::uint64_t>(
                    section.VirtualAddress) + fileOffset - rawStart;
                if (rva > std::numeric_limits<DWORD>::max())
                    throw std::runtime_error("invalid PE: patch RVA is out of range");
                return static_cast<DWORD>(rva);
            }
        }
        throw std::runtime_error("patch signature is outside mapped PE sections");
    }

    BYTE* remoteImageBase(HANDLE process)
    {
        using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE,
            PROCESSINFOCLASS, PVOID, ULONG, PULONG);
        const auto query = reinterpret_cast<NtQueryInformationProcessFn>(GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (!query)
            throw std::runtime_error("NtQueryInformationProcess is unavailable");

        PROCESS_BASIC_INFORMATION information{};
        const NTSTATUS status = query(process, ProcessBasicInformation, &information,
            sizeof(information), nullptr);
        if (status < 0)
            throw std::runtime_error("NtQueryInformationProcess failed");

        static_assert(sizeof(void*) == 8, "the launcher must be built for x64");
        PVOID imageBase{};
        SIZE_T bytesRead{};
        const auto imageBaseField = reinterpret_cast<const BYTE*>(
            information.PebBaseAddress) + 0x10;
        if (!ReadProcessMemory(process, imageBaseField, &imageBase, sizeof(imageBase),
            &bytesRead) || bytesRead != sizeof(imageBase))
            throw std::runtime_error("cannot read child process image base: " +
                windowsError(GetLastError()));
        return static_cast<BYTE*>(imageBase);
    }

    void writeRemotePatch(HANDLE process, BYTE* address,
        const std::vector<BYTE>& replacement, const char* name)
    {
        DWORD oldProtection{};
        if (!VirtualProtectEx(process, address, replacement.size(), PAGE_EXECUTE_READWRITE,
            &oldProtection))
            throw std::runtime_error(std::string("cannot change protection for patch ") +
                name + ": " + windowsError(GetLastError()));

        SIZE_T written{};
        const BOOL writeSucceeded = WriteProcessMemory(process, address,
            replacement.data(), replacement.size(), &written);
        const DWORD writeError = writeSucceeded ? ERROR_SUCCESS : GetLastError();
        DWORD ignored{};
        const BOOL restoreSucceeded = VirtualProtectEx(process, address,
            replacement.size(), oldProtection, &ignored);
        const DWORD restoreError = restoreSucceeded ? ERROR_SUCCESS : GetLastError();

        if (!writeSucceeded || written != replacement.size())
            throw std::runtime_error(std::string("cannot write patch ") + name + ": " +
                windowsError(writeError));
        if (!restoreSucceeded)
            throw std::runtime_error(std::string("cannot restore protection for patch ") +
                name + ": " + windowsError(restoreError));
        if (!FlushInstructionCache(process, address, replacement.size()))
            throw std::runtime_error(std::string("cannot flush instruction cache for patch ") +
                name + ": " + windowsError(GetLastError()));
    }

    void patchDayZServer(HANDLE process, const fs::path& target)
    {
        const std::array<Patch, 2> patches{{
            {
                "force init success",
                { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC,
                    -1, -1, -1, -1, 0x45, 0x33, 0xE4, 0x48, 0x8B, 0xD9, 0x44, 0x89 },
                { 0xB0, 0x01, 0xC3 }
            },
            {
                "change conditional jump",
                { 0x74, 0x44, 0x0F, 0xB7, 0xC8, 0xE8, -1, -1, -1, -1, 0x8B,
                    0x13, 0x44, 0x0F, 0xB7, 0xC0, 0x44, 0x89, 0x4C, 0x24, -1, 0x48 },
                { 0xEB }
            }
        }};

        const auto fileBytes = readFile(target);
        BYTE* const imageBase = remoteImageBase(process);
        for (const auto& patch : patches)
        {
            const std::size_t fileOffset = findUniquePattern(fileBytes, patch);
            const DWORD rva = fileOffsetToRva(fileBytes, fileOffset);
            writeRemotePatch(process, imageBase + rva, patch.replacement, patch.name);
        }
    }

    BOOL WINAPI consoleControlHandler(DWORD event)
    {
        // The child shares this console and receives Ctrl+C/Ctrl+Break itself. Keep the
        // wrapper alive long enough to observe and return the server's actual exit code.
        return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
    }

    void usage()
    {
        std::wcout <<
            L"Usage: dayz_server_launcher [launcher options] [--] [DayZ server arguments]\n\n"
            L"Launcher options:\n"
            L"  --launcher-diag          Run sibling DayZDiag_x64.exe and ensure -server\n"
            L"  --launcher-target <exe>  Run the specified DayZServer_x64.exe or DayZDiag_x64.exe\n"
            L"  --launcher-dry-run       Print the resolved command without starting it\n"
            L"  --launcher-help          Show this help\n"
            L"  --                       Forward all remaining arguments verbatim\n\n"
            L"Without launcher options, a sibling DayZServer_x64.exe is started.\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        SetConsoleTitleW(L"DayZ VR server launcher - No BE, VR mod");
        const fs::path self = launcherPath();
        fs::path target = defaultTarget(self, L"DayZServer_x64.exe");
        bool dryRun = false;
        bool parseLauncherOptions = true;
        std::vector<std::wstring> forwarded;

        for (int index = 1; index < argc; ++index)
        {
            const std::wstring_view argument(argv[index]);
            if (parseLauncherOptions && argument == L"--")
            {
                parseLauncherOptions = false;
                continue;
            }
            if (parseLauncherOptions && argument == L"--launcher-help")
            {
                usage();
                return 0;
            }
            if (parseLauncherOptions && argument == L"--launcher-diag")
            {
                target = defaultTarget(self, L"DayZDiag_x64.exe");
                continue;
            }
            if (parseLauncherOptions && argument == L"--launcher-target")
            {
                if (++index >= argc)
                    throw std::runtime_error("--launcher-target requires an executable path");
                target = argv[index];
                continue;
            }
            if (parseLauncherOptions && argument == L"--launcher-dry-run")
            {
                dryRun = true;
                continue;
            }
            forwarded.emplace_back(argument);
        }

        target = fs::absolute(target).lexically_normal();
        const auto targetName = lowercase(target.filename().wstring());
        const bool diagnostic = targetName == L"dayzdiag_x64.exe";
        if (!diagnostic && targetName != L"dayzserver_x64.exe")
            throw std::runtime_error("target must be DayZServer_x64.exe or DayZDiag_x64.exe");
        if (!fs::is_regular_file(target))
            throw std::runtime_error("target executable does not exist: " + target.string());
        if (fs::equivalent(self, target))
            throw std::runtime_error("launcher target resolves to the launcher itself");

        if (diagnostic)
        {
            const bool hasServerArgument = std::any_of(forwarded.begin(), forwarded.end(),
                [](const std::wstring& value) { return lowercase(value) == L"-server"; });
            if (!hasServerArgument) forwarded.emplace_back(L"-server");
        }

        std::wstring commandLine = buildCommandLine(target, forwarded);
        std::wcout << L"Starting: " << commandLine << L'\n';
        if (dryRun) return 0;

        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};

        if (!SetConsoleCtrlHandler(consoleControlHandler, TRUE))
            throw std::runtime_error("SetConsoleCtrlHandler failed: " +
                windowsError(GetLastError()));
        const DWORD creationFlags = diagnostic ? 0 : CREATE_SUSPENDED;
        if (!CreateProcessW(target.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            creationFlags,
            nullptr, nullptr, &startup, &process))
            throw std::runtime_error("CreateProcessW failed: " + windowsError(GetLastError()));

        if (!diagnostic)
        {
            try
            {
                patchDayZServer(process.hProcess, target);
                if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
                    throw std::runtime_error("ResumeThread failed: " +
                        windowsError(GetLastError()));
                std::wcout << L"Applied DayZServer_x64.exe compatibility patches.\n";
            }
            catch (...)
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                throw;
            }
        }
        CloseHandle(process.hThread);
        const DWORD waitResult = WaitForSingleObject(process.hProcess, INFINITE);
        if (waitResult != WAIT_OBJECT_0)
        {
            const DWORD error = GetLastError();
            CloseHandle(process.hProcess);
            throw std::runtime_error("waiting for server failed: " + windowsError(error));
        }

        DWORD exitCode{};
        if (!GetExitCodeProcess(process.hProcess, &exitCode))
        {
            const DWORD error = GetLastError();
            CloseHandle(process.hProcess);
            throw std::runtime_error("GetExitCodeProcess failed: " + windowsError(error));
        }
        CloseHandle(process.hProcess);
        return static_cast<int>(exitCode);
    }
    catch (const std::exception& error)
    {
        std::cerr << "dayz_server_launcher: " << error.what() << '\n';
        return 1;
    }
}
