#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <malloc.h>
#include <sched.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "Containers/Vector.h"
#include "Containers/String.h"
#include "Log/Log.h"
#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"

extern char** environ;

namespace Lumina::Platform
{
    namespace
    {
        TVector<FString> GDLLSearchPaths;

        FString ReadWholeFile(const char* Path)
        {
            FString Contents;

            const int Descriptor = ::open(Path, O_RDONLY | O_CLOEXEC);

            if (Descriptor < 0)
            {
                return Contents;
            }

            char Buffer[4096];
            ssize_t Read;

            while ((Read = ::read(Descriptor, Buffer, sizeof(Buffer))) > 0)
            {
                Contents.append(Buffer, static_cast<size_t>(Read));
            }

            ::close(Descriptor);

            return Contents;
        }

        FString ReadSysfsValue(const char* Path)
        {
            FString Value = ReadWholeFile(Path);

            const size_t Newline = Value.find('\n');

            if (Newline != FString::npos)
            {
                Value.resize(Newline);
            }

            return Value;
        }

        TVector<FString> TokenizeArguments(const TCHAR* Params)
        {
            TVector<FString> Arguments;

            if (Params == nullptr)
            {
                return Arguments;
            }

            FString Current;
            bool bHasToken = false;
            bool bInQuotes = false;

            for (const char* Cursor = Params; *Cursor != '\0'; ++Cursor)
            {
                if (*Cursor == '\\')
                {
                    size_t Count = 0;

                    while (Cursor[Count] == '\\')
                    {
                        ++Count;
                    }

                    if (Cursor[Count] == '"')
                    {
                        Current.append(Count / 2, '\\');

                        if (Count % 2 == 1)
                        {
                            Current.push_back('"');
                            Cursor += Count;
                        }
                        else
                        {
                            bInQuotes = !bInQuotes;
                            Cursor += Count;
                        }

                        bHasToken = true;
                    }
                    else
                    {
                        Current.append(Count, '\\');
                        Cursor += Count - 1;
                        bHasToken = true;
                    }

                    continue;
                }

                if (*Cursor == '"')
                {
                    bInQuotes = !bInQuotes;
                    bHasToken = true;
                    continue;
                }

                if (!bInQuotes && (*Cursor == ' ' || *Cursor == '\t'))
                {
                    if (bHasToken)
                    {
                        Arguments.push_back(Current);
                        Current.clear();
                        bHasToken = false;
                    }

                    continue;
                }

                Current.push_back(*Cursor);
                bHasToken = true;
            }

            if (bHasToken)
            {
                Arguments.push_back(Current);
            }

            return Arguments;
        }

        TVector<char*> BuildArgv(const FString& Program, const TVector<FString>& Arguments, TVector<FString>& Storage)
        {
            Storage.clear();
            Storage.reserve(Arguments.size() + 1);
            Storage.push_back(Program);

            for (const FString& Argument : Arguments)
            {
                Storage.push_back(Argument);
            }

            TVector<char*> Argv;
            Argv.reserve(Storage.size() + 1);

            for (FString& Entry : Storage)
            {
                Argv.push_back(Entry.data());
            }

            Argv.push_back(nullptr);

            return Argv;
        }

        FString FindOnPath(const char* Program)
        {
            if (::strchr(Program, '/') != nullptr)
            {
                return ::access(Program, X_OK) == 0 ? FString(Program) : FString();
            }

            const char* PathValue = ::getenv("PATH");

            if (PathValue == nullptr)
            {
                return FString();
            }

            FString Remaining(PathValue);
            size_t Start = 0;

            while (Start <= Remaining.size())
            {
                size_t Separator = Remaining.find(':', Start);

                if (Separator == FString::npos)
                {
                    Separator = Remaining.size();
                }

                if (Separator > Start)
                {
                    FString Candidate(Remaining.data() + Start, Separator - Start);
                    Candidate += '/';
                    Candidate += Program;

                    if (::access(Candidate.c_str(), X_OK) == 0)
                    {
                        return Candidate;
                    }
                }

                Start = Separator + 1;
            }

            return FString();
        }

        int SpawnDetached(const FString& Program, const TVector<FString>& Arguments)
        {
            TVector<FString> Storage;
            TVector<char*> Argv = BuildArgv(Program, Arguments, Storage);

            const pid_t Intermediate = ::fork();

            if (Intermediate < 0)
            {
                return errno;
            }

            if (Intermediate == 0)
            {
                ::setsid();

                if (::fork() == 0)
                {
                    ::execv(Argv[0], Argv.data());
                    ::_exit(127);
                }

                ::_exit(0);
            }

            int Status = 0;

            while (::waitpid(Intermediate, &Status, 0) < 0 && errno == EINTR)
            {
            }

            return 0;
        }

        int SpawnAndWait(
            const FString& Program,
            const TVector<FString>& Arguments,
            const TCHAR* WorkingDirectory,
            const TFunction<void(FStringView)>* LineCallback)
        {
            int Pipe[2] = { -1, -1 };

            if (LineCallback != nullptr && ::pipe2(Pipe, O_CLOEXEC) != 0)
            {
                LOG_ERROR("Failed to create a pipe for '{0}': {1}", Program, ::strerror(errno));
                return -1;
            }

            posix_spawn_file_actions_t Actions;
            posix_spawn_file_actions_init(&Actions);

            if (LineCallback != nullptr)
            {
                posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDOUT_FILENO);
                posix_spawn_file_actions_adddup2(&Actions, Pipe[1], STDERR_FILENO);
                posix_spawn_file_actions_addclose(&Actions, Pipe[0]);
            }

            if (WorkingDirectory != nullptr && WorkingDirectory[0] != '\0')
            {
                posix_spawn_file_actions_addchdir_np(&Actions, WorkingDirectory);
            }

            TVector<FString> Storage;
            TVector<char*> Argv = BuildArgv(Program, Arguments, Storage);

            pid_t Child = -1;
            const int SpawnResult = ::posix_spawn(&Child, Program.c_str(), &Actions, nullptr, Argv.data(), environ);

            posix_spawn_file_actions_destroy(&Actions);

            if (SpawnResult != 0)
            {
                LOG_ERROR("Failed to start '{0}': {1}", Program, ::strerror(SpawnResult));

                if (Pipe[0] >= 0)
                {
                    ::close(Pipe[0]);
                    ::close(Pipe[1]);
                }

                return -1;
            }

            if (LineCallback != nullptr)
            {
                ::close(Pipe[1]);
                Pipe[1] = -1;

                FString Pending;
                char Buffer[4096];
                ssize_t Read;

                while ((Read = ::read(Pipe[0], Buffer, sizeof(Buffer))) != 0)
                {
                    if (Read < 0)
                    {
                        if (errno == EINTR)
                        {
                            continue;
                        }

                        break;
                    }

                    Pending.append(Buffer, static_cast<size_t>(Read));

                    size_t LineStart = 0;
                    size_t Newline;

                    while ((Newline = Pending.find('\n', LineStart)) != FString::npos)
                    {
                        size_t LineEnd = Newline;

                        if (LineEnd > LineStart && Pending[LineEnd - 1] == '\r')
                        {
                            --LineEnd;
                        }

                        (*LineCallback)(FStringView(Pending.data() + LineStart, LineEnd - LineStart));
                        LineStart = Newline + 1;
                    }

                    Pending.erase(0, LineStart);
                }

                if (!Pending.empty())
                {
                    (*LineCallback)(FStringView(Pending.data(), Pending.size()));
                }

                ::close(Pipe[0]);
            }

            int Status = 0;

            while (::waitpid(Child, &Status, 0) < 0)
            {
                if (errno != EINTR)
                {
                    return -1;
                }
            }

            if (WIFEXITED(Status))
            {
                return WEXITSTATUS(Status);
            }

            return WIFSIGNALED(Status) ? 128 + WTERMSIG(Status) : -1;
        }

        bool LaunchDesktopHelper(const char* Program, const TVector<FString>& Arguments)
        {
            const FString Resolved = FindOnPath(Program);

            if (Resolved.empty())
            {
                return false;
            }

            return SpawnDetached(Resolved, Arguments) == 0;
        }
    }


    void* GetDLLHandle(const TCHAR* Filename)
    {
        if (Filename == nullptr)
        {
            return nullptr;
        }

        return LoadLibraryWithSearchPaths(FString(Filename), GDLLSearchPaths);
    }

    bool FreeDLLHandle(void* DLLHandle)
    {
        return DLLHandle != nullptr && ::dlclose(DLLHandle) == 0;
    }

    void* GetDLLExport(void* DLLHandle, const char* ProcName)
    {
        return DLLHandle != nullptr ? ::dlsym(DLLHandle, ProcName) : nullptr;
    }

    void AddDLLDirectory(const FString& Directory)
    {
        for (const FString& Existing : GDLLSearchPaths)
        {
            if (Existing == Directory)
            {
                return;
            }
        }

        GDLLSearchPaths.push_back(Directory);
    }

    void PushDLLDirectory(const TCHAR* Directory)
    {
        if (Directory == nullptr)
        {
            return;
        }

        GDLLSearchPaths.push_back(FString(Directory));
        LOG_INFO("Pushing DLL Search Path: {0}", Directory);
    }

    void PopDLLDirectory()
    {
        if (!GDLLSearchPaths.empty())
        {
            GDLLSearchPaths.pop_back();
        }
    }

    void* LoadLibraryWithSearchPaths(const FString& Filename, const TVector<FString>& SearchPaths)
    {
        if (void* Existing = ::dlopen(Filename.c_str(), RTLD_NOLOAD | RTLD_LAZY))
        {
            return Existing;
        }

        if (void* Handle = ::dlopen(Filename.c_str(), RTLD_LAZY | RTLD_LOCAL))
        {
            return Handle;
        }

        const char* Reported = ::dlerror();
        const FString FirstError = Reported != nullptr ? FString(Reported) : FString();

        for (const FString& Path : SearchPaths)
        {
            FFixedString FullPath = Paths::Combine(Path, Filename);

            if (!Paths::Exists(FullPath))
            {
                continue;
            }

            if (void* Handle = ::dlopen(FullPath.c_str(), RTLD_LAZY | RTLD_LOCAL))
            {
                return Handle;
            }

            const char* Error = ::dlerror();
            LOG_WARN("dlopen failed for '{0}': {1}", FullPath.c_str(), Error != nullptr ? Error : "unknown");
        }

        LOG_ERROR("dlopen failed for '{0}': {1}", Filename, FirstError);

        return nullptr;
    }

    FVoidFuncPtr LumGetProcAddress(void* Handle, const char* Procedure)
    {
        return reinterpret_cast<FVoidFuncPtr>(::dlsym(Handle, Procedure));
    }


    uint32 GetCurrentCoreNumber()
    {
        const int Core = ::sched_getcpu();
        return Core < 0 ? 0u : static_cast<uint32>(Core);
    }

    FString GetCurrentProcessPath()
    {
        char Buffer[PATH_MAX];
        const ssize_t Length = ::readlink("/proc/self/exe", Buffer, sizeof(Buffer) - 1);

        if (Length <= 0)
        {
            return FString();
        }

        return FString(Buffer, static_cast<size_t>(Length));
    }

    const TCHAR* BaseDir()
    {
        static char Buffer[PATH_MAX] = {};

        if (Buffer[0] == 0)
        {
            const ssize_t Length = ::readlink("/proc/self/exe", Buffer, sizeof(Buffer) - 1);

            if (Length > 0)
            {
                Buffer[Length] = '\0';
            }
        }

        return Buffer;
    }

    const TCHAR* ExecutableName(bool bRemoveExtension)
    {
        static char Buffer[PATH_MAX];

        const ssize_t Length = ::readlink("/proc/self/exe", Buffer, sizeof(Buffer) - 1);

        if (Length <= 0)
        {
            return nullptr;
        }

        Buffer[Length] = '\0';

        char* Name = ::strrchr(Buffer, '/');
        Name = Name != nullptr ? Name + 1 : Buffer;

        if (bRemoveExtension)
        {
            if (char* Dot = ::strrchr(Name, '.'))
            {
                *Dot = '\0';
            }
        }

        return Name;
    }


    const FCpuTopology& GetCpuTopology()
    {
        static FCpuTopology Topology = []
        {
            FCpuTopology T;

            for (ECpuCoreType& Type : T.CoreTypes)
            {
                Type = ECpuCoreType::Unknown;
            }

            const long Online = ::sysconf(_SC_NPROCESSORS_ONLN);
            T.NumLogicalCores = Online > 0 ? static_cast<uint32>(Online) : 1u;

            uint64 Capacities[256] = {};
            uint64 MaxCapacity = 0;
            bool bHaveCapacities = false;

            const uint32 Count = T.NumLogicalCores < 256 ? T.NumLogicalCores : 256;

            for (uint32 Index = 0; Index < Count; ++Index)
            {
                char Path[128];
                ::snprintf(Path, sizeof(Path), "/sys/devices/system/cpu/cpu%u/cpu_capacity", Index);

                const FString Value = ReadSysfsValue(Path);

                if (Value.empty())
                {
                    continue;
                }

                Capacities[Index] = ::strtoull(Value.c_str(), nullptr, 10);
                bHaveCapacities = bHaveCapacities || Capacities[Index] > 0;
                MaxCapacity = Capacities[Index] > MaxCapacity ? Capacities[Index] : MaxCapacity;
            }

            if (!bHaveCapacities || MaxCapacity == 0)
            {
                for (uint32 Index = 0; Index < Count; ++Index)
                {
                    T.CoreTypes[Index] = ECpuCoreType::Performance;
                }

                T.NumPerformance = T.NumLogicalCores;
                T.bHybrid = false;

                return T;
            }

            for (uint32 Index = 0; Index < Count; ++Index)
            {
                if (Capacities[Index] >= MaxCapacity)
                {
                    T.CoreTypes[Index] = ECpuCoreType::Performance;
                    ++T.NumPerformance;
                }
                else
                {
                    T.CoreTypes[Index] = ECpuCoreType::Efficiency;
                    ++T.NumEfficiency;
                }
            }

            T.bHybrid = T.NumEfficiency > 0;

            return T;
        }();

        return Topology;
    }


    FString GetEnvVariable(FStringView Variable)
    {
        const FString Name(Variable);
        const char* Value = ::getenv(Name.c_str());
        return Value ? FString(Value) : FString();
    }

    bool SetEnvVariable(const FString& Name, const FString& Value)
    {
        if (Name.empty() || Name.find('=') != FString::npos)
        {
            LOG_WARN("Refusing to set environment variable with invalid name '{}'", Name);
            return false;
        }

        // Windows cannot store an empty variable, so removing on empty keeps the platforms observably equal.
        const int Result = Value.empty() ? ::unsetenv(Name.c_str()) : ::setenv(Name.c_str(), Value.c_str(), 1);
        if (Result == 0)
        {
            LOG_TRACE("Environment variable {} set to {}", Name, Value);
            return true;
        }

        LOG_WARN("Failed to set environment variable {}", Name);

        return false;
    }

    bool PersistUserEnvVariable(const FString& Name, const FString& Value)
    {
        static bool bWarned = false;

        if (!bWarned)
        {
            bWarned = true;
            LOG_WARN("Cannot persist {}={} : there is no user environment store on this platform. "
                     "Add it to your shell profile to make it permanent.", Name, Value);
        }

        return false;
    }



    int LaunchProcess(const TCHAR* URL, const TCHAR* Params, bool bLaunchDetached)
    {
        if (URL == nullptr)
        {
            return -1;
        }

        const TVector<FString> Arguments = TokenizeArguments(Params);

        if (bLaunchDetached)
        {
            return SpawnDetached(FString(URL), Arguments);
        }

        TVector<FString> Storage;
        TVector<char*> Argv = BuildArgv(FString(URL), Arguments, Storage);

        pid_t Child = -1;
        const int Result = ::posix_spawn(&Child, URL, nullptr, nullptr, Argv.data(), environ);

        return Result == 0 ? 0 : Result;
    }

    void LaunchURL(const TCHAR* URL)
    {
        if (URL == nullptr)
        {
            return;
        }

        if (!LaunchDesktopHelper("xdg-open", { FString(URL) }))
        {
            LOG_WARN("Could not open '{0}': xdg-open is not installed.", URL);
        }
    }

    int RunProcessAndWait(const TCHAR* Executable, const TCHAR* Params, const TCHAR* WorkingDirectory)
    {
        if (Executable == nullptr)
        {
            return -1;
        }

        return SpawnAndWait(FString(Executable), TokenizeArguments(Params), WorkingDirectory, nullptr);
    }

    int RunProcessAndWaitCapture(
        const TCHAR* Executable,
        const TCHAR* Params,
        const TCHAR* WorkingDirectory,
        const TFunction<void(FStringView)>& LineCallback)
    {
        if (Executable == nullptr)
        {
            return -1;
        }

        return SpawnAndWait(FString(Executable), TokenizeArguments(Params), WorkingDirectory, &LineCallback);
    }


    size_t GetProcessMemoryUsageBytes()
    {
        const FString Statm = ReadWholeFile("/proc/self/statm");

        if (Statm.empty())
        {
            return 0;
        }

        unsigned long long Total = 0;
        unsigned long long Resident = 0;

        if (::sscanf(Statm.c_str(), "%llu %llu", &Total, &Resident) != 2)
        {
            return 0;
        }

        return static_cast<size_t>(Resident) * static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    }

    size_t GetProcessMemoryUsageMegaBytes()
    {
        return GetProcessMemoryUsageBytes() / (1024 * 1024);
    }

    void GetAddressSpaceStats(FAddressSpaceStats& Out, bool bIncludeHeaps)
    {
        Out = FAddressSpaceStats();

        const FString Smaps = ReadWholeFile("/proc/self/smaps");

        if (Smaps.empty())
        {
            return;
        }

        enum class ERegionKind : uint8 { Private, Image, Mapped, Reserved };

        ERegionKind Kind = ERegionKind::Private;
        bool bInRegion = false;

        size_t LineStart = 0;

        while (LineStart < Smaps.size())
        {
            size_t LineEnd = Smaps.find('\n', LineStart);

            if (LineEnd == FString::npos)
            {
                LineEnd = Smaps.size();
            }

            const char* Line = Smaps.data() + LineStart;
            const size_t LineLength = LineEnd - LineStart;

            const bool bIsHeader = LineLength > 0 && ::isxdigit(static_cast<unsigned char>(Line[0]));

            if (bIsHeader)
            {
                ++Out.RegionCount;
                bInRegion = true;

                unsigned long long Start = 0;
                unsigned long long End = 0;
                char Permissions[8] = {};
                unsigned long long Offset = 0;
                char Device[16] = {};
                unsigned long long Inode = 0;
                int PathOffset = 0;

                const int Fields = ::sscanf(Line, "%llx-%llx %7s %llx %15s %llu %n",
                    &Start, &End, Permissions, &Offset, Device, &Inode, &PathOffset);

                if (Fields < 6)
                {
                    bInRegion = false;
                    LineStart = LineEnd + 1;
                    continue;
                }

                const bool bAnonymous = Inode == 0;
                const bool bExecutable = ::strchr(Permissions, 'x') != nullptr;
                const bool bUnreadable = Permissions[0] == '-' && Permissions[1] == '-' && Permissions[2] == '-';

                if (bUnreadable)
                {
                    Kind = ERegionKind::Reserved;
                    Out.Reserved += End - Start;
                }
                else if (bAnonymous)
                {
                    Kind = ERegionKind::Private;
                }
                else
                {
                    Kind = bExecutable ? ERegionKind::Image : ERegionKind::Mapped;
                }
            }
            else if (bInRegion && LineLength > 4 && ::strncmp(Line, "Rss:", 4) == 0)
            {
                const unsigned long long Kilobytes = ::strtoull(Line + 4, nullptr, 10);
                const uint64 Bytes = static_cast<uint64>(Kilobytes) * 1024ull;

                switch (Kind)
                {
                case ERegionKind::Private:  Out.PrivateCommitted += Bytes; break;
                case ERegionKind::Image:    Out.ImageCommitted += Bytes;   break;
                case ERegionKind::Mapped:   Out.MappedCommitted += Bytes;  break;
                case ERegionKind::Reserved: break;
                }
            }

            LineStart = LineEnd + 1;
        }

        if (!bIncludeHeaps)
        {
            return;
        }

        const struct mallinfo2 Info = ::mallinfo2();

        Out.HeapCommitted = static_cast<uint64>(Info.arena) + static_cast<uint64>(Info.hblkhd);
        Out.HeapAllocated = static_cast<uint64>(Info.uordblks);
        Out.HeapOverhead = Out.HeapCommitted > Out.HeapAllocated ? Out.HeapCommitted - Out.HeapAllocated : 0;
        Out.HeapCount = 1;
        Out.bHeapWalkValid = true;
    }


    void ShowFileInExplorer(const TCHAR* Path)
    {
        if (Path == nullptr)
        {
            return;
        }

        if (LaunchDesktopHelper("dbus-send", {
                "--session",
                "--dest=org.freedesktop.FileManager1",
                "--type=method_call",
                "/org/freedesktop/FileManager1",
                "org.freedesktop.FileManager1.ShowItems",
                FString("array:string:file://") + Path,
                "string:\"\"" }))
        {
            return;
        }

        const FString Directory = Paths::Parent(Path);
        ShowFolderInExplorer(Directory.c_str());
    }

    void ShowFolderInExplorer(const TCHAR* Directory)
    {
        if (Directory == nullptr)
        {
            return;
        }

        if (!LaunchDesktopHelper("xdg-open", { FString(Directory) }))
        {
            LOG_WARN("Could not open '{0}': xdg-open is not installed.", Directory);
        }
    }

    void OpenSourceFile(const TCHAR* Path, int32 Line)
    {
        if (Path == nullptr)
        {
            return;
        }

        if (Line > 0)
        {
            const FString LineText = Format("{0}", Line);
            if (LaunchDesktopHelper("rider", { FString("--line"), LineText, FString(Path) }))
            {
                return;
            }
            if (LaunchDesktopHelper("code", { FString("--goto"), FString(Path) + ":" + LineText }))
            {
                return;
            }
        }

        if (!LaunchDesktopHelper("xdg-open", { FString(Path) }))
        {
            LOG_WARN("Could not open '{0}': xdg-open is not installed.", Path);
        }
    }

    void OpenTerminalAt(const TCHAR* Directory)
    {
        if (Directory == nullptr)
        {
            return;
        }

        static const struct { const char* Program; const char* Flag; } Terminals[] =
        {
            { "x-terminal-emulator", "--working-directory=" },
            { "gnome-terminal",      "--working-directory=" },
            { "konsole",             "--workdir=" },
            { "xfce4-terminal",      "--working-directory=" },
            { "alacritty",           "--working-directory=" },
            { "kitty",               "--directory=" },
            { "xterm",               nullptr },
        };

        for (const auto& Terminal : Terminals)
        {
            TVector<FString> Arguments;

            if (Terminal.Flag != nullptr)
            {
                Arguments.push_back(FString(Terminal.Flag) + Directory);
            }

            if (LaunchDesktopHelper(Terminal.Program, Arguments))
            {
                return;
            }
        }

        static bool bWarned = false;

        if (!bWarned)
        {
            bWarned = true;
            LOG_WARN("Platform::OpenTerminalAt: no supported terminal emulator was found.");
        }
    }


    namespace
    {
        void AppendFilterArguments(const char* Filter, TVector<FString>& Arguments)
        {
            if (Filter == nullptr)
            {
                return;
            }

            const char* Cursor = Filter;

            while (*Cursor != '\0')
            {
                const FString Description(Cursor);
                Cursor += Description.size() + 1;

                if (*Cursor == '\0')
                {
                    break;
                }

                const FString Patterns(Cursor);
                Cursor += Patterns.size() + 1;

                FString Translated = Patterns;
                Algo::Replace(Translated, ';', ' ');

                Arguments.push_back(FString("--file-filter=") + Description + " | " + Translated);
            }
        }

        bool RunFileChooser(
            const char* Title,
            const char* Filter,
            const char* InitialDir,
            bool bMultiple,
            TVector<FFixedString>& OutPaths)
        {
            const FString Zenity = FindOnPath("zenity");

            if (Zenity.empty())
            {
                static bool bWarned = false;

                if (!bWarned)
                {
                    bWarned = true;
                    LOG_WARN("Platform file dialogs need zenity, which is not installed.");
                }

                return false;
            }

            TVector<FString> Arguments;
            Arguments.push_back("--file-selection");

            if (Filter == nullptr)
            {
                Arguments.push_back("--directory");
            }
            else
            {
                AppendFilterArguments(Filter, Arguments);
            }

            if (bMultiple)
            {
                Arguments.push_back("--multiple");
                Arguments.push_back("--separator=\n");
            }

            if (Title != nullptr)
            {
                Arguments.push_back(FString("--title=") + Title);
            }

            if (InitialDir != nullptr && InitialDir[0] != '\0')
            {
                FString Start(InitialDir);

                if (Start.back() != '/')
                {
                    Start += '/';
                }

                Arguments.push_back(FString("--filename=") + Start);
            }

            TVector<FFixedString> Selected;

            const TFunction<void(FStringView)> Collect = [&Selected](FStringView Line)
            {
                if (!Line.empty())
                {
                    Selected.push_back(FFixedString(Line.data(), Line.size()));
                }
            };

            const int ExitCode = SpawnAndWait(Zenity, Arguments, nullptr, &Collect);

            if (ExitCode != 0 || Selected.empty())
            {
                return false;
            }

            OutPaths = std::move(Selected);

            return true;
        }
    }

    bool OpenFileDialogue(FFixedString& OutFile, const char* Title, const char* Filter, const char* InitialDir)
    {
        TVector<FFixedString> Selected;

        if (!RunFileChooser(Title, Filter, InitialDir, false, Selected))
        {
            return false;
        }

        OutFile = Selected.front();

        return true;
    }

    bool OpenFileDialogueMulti(TVector<FFixedString>& OutFiles, const char* Title, const char* Filter, const char* InitialDir)
    {
        TVector<FFixedString> Selected;

        if (!RunFileChooser(Title, Filter, InitialDir, true, Selected))
        {
            return false;
        }

        OutFiles = std::move(Selected);

        return true;
    }
}

#endif
