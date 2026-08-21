#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#ifdef LE_PLATFORM_WINDOWS

#include "Platform/CrashReporter.h"

#include "Lumina.h"
#include "Log/Log.h"
#include "Platform/Filesystem/FileHelper.h"
#include "Platform/Process/PlatformProcess.h"

#include <atomic>
#include "Platform/Filesystem/PlatformFilesystem.h"

#if WITH_BUGSPLAT
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Only the C ABI is exported, and it is CRT-agnostic, so one DLL serves every configuration.
#include <BugSplatC.h>
#endif

namespace Lumina::CrashReporting
{
    namespace
    {
        std::atomic<bool> GInitialized{ false };

        // Filled by Initialize, drained by LogStatus once logging exists.
        FString GStatusLine;

        #if WITH_BUGSPLAT
        constexpr uint32 GMaxWideChars = 512;

        // A truncated path attaches the wrong file, so anything oversized is dropped rather than cut.
        bool Widen(FStringView In, wchar_t* Out, uint32 OutChars)
        {
            if (In.empty() || In.size() >= OutChars)
            {
                return false;
            }

            const int Written = MultiByteToWideChar(CP_UTF8, 0, In.data(), static_cast<int>(In.size()),
                Out, static_cast<int>(OutChars - 1));
            if (Written <= 0)
            {
                return false;
            }

            Out[Written] = 0;
            return true;
        }

        // Never torn down, since it owns the filter for the life of the process.
        std::atomic<bool> GSenderReady{ false };

        // Tracked here because the C API has RemoveAttachment but no ClearAttachments.
        FMutex          GAttachmentMutex;
        TVector<FWString>   GAttachments;

        // Read from .git at runtime, since everyone builds from their own commit and baking it in rebuilds the world.
        FString ReadGitCommit()
        {
            auto ParentOf = [](FStringView Path) -> FString
            {
                const size_t Slash = Path.find_last_of("/\\");
                return Slash == FStringView::npos ? FString() : FString(Path.data(), Slash);
            };

            // Paths is not initialized this early, so the exe-relative walk is repeated here rather than shared.
            const FString Root = ParentOf(ParentOf(ParentOf(Platform::GetCurrentProcessPath())));
            const FString GitDir = Root + "/.git";

            FString Head;
            if (!Filesystem::ReadFile(Head, GitDir + "/HEAD"))
            {
                // A packaged build with no working tree. Symbols match properly for those anyway.
                return {};
            }

            while (!Head.empty() && (Head.back() == '\n' || Head.back() == '\r' || Head.back() == ' '))
            {
                Head.pop_back();
            }

            // Attached HEAD points at a ref; detached HEAD is the hash itself.
            constexpr FStringView RefPrefix = "ref: ";
            if (FStringView(Head).starts_with(RefPrefix))
            {
                const FString RefName = Head.substr(RefPrefix.size());

                FString Resolved;
                if (!Filesystem::ReadFile(Resolved, GitDir + "/" + RefName))
                {
                    // git packs refs during gc, so a missing loose ref is the normal state on a user's machine.
                    FString Packed;
                    if (!Filesystem::ReadFile(Packed, GitDir + "/packed-refs"))
                    {
                        return {};
                    }

                    // Lines are "<sha> <refname>". '#' is the header, '^' a peeled tag target.
                    size_t LineStart = 0;
                    while (LineStart < Packed.size())
                    {
                        size_t LineEnd = Packed.find('\n', LineStart);
                        if (LineEnd == FString::npos)
                        {
                            LineEnd = Packed.size();
                        }

                        const FStringView Line(Packed.data() + LineStart, LineEnd - LineStart);
                        LineStart = LineEnd + 1;

                        if (Line.empty() || Line.front() == '#' || Line.front() == '^')
                        {
                            continue;
                        }

                        const size_t Space = Line.find(' ');
                        if (Space == FStringView::npos)
                        {
                            continue;
                        }

                        FStringView Name = Line.substr(Space + 1);
                        while (!Name.empty() && (Name.back() == '\r' || Name.back() == ' '))
                        {
                            Name = Name.substr(0, Name.size() - 1);
                        }

                        if (Name == FStringView(RefName))
                        {
                            Resolved.assign(Line.data(), Space);
                            break;
                        }
                    }

                    if (Resolved.empty())
                    {
                        return {};
                    }
                }

                Head = Resolved;
                while (!Head.empty() && (Head.back() == '\n' || Head.back() == '\r'))
                {
                    Head.pop_back();
                }
            }

            // Enough to identify the commit, short enough to read in a version column.
            return Head.size() >= 8 ? Head.substr(0, 8) : FString();
        }
        #endif
    }


    void Initialize()
    {
        if (GInitialized.exchange(true))
        {
            return;
        }

        #if WITH_BUGSPLAT
        // An editor built from source is only identifiable by the tree it came from.
        FString VersionString = FString(LUMINA_VERSION) + "-" + LUMINA_CONFIGURATION_NAME;

        if (const FString Commit = ReadGitCommit(); !Commit.empty())
        {
            VersionString += "-";
            VersionString += Commit;
        }

        wchar_t Database[GMaxWideChars];
        wchar_t Version[GMaxWideChars];

        if (!Widen(BUGSPLAT_DATABASE, Database, GMaxWideChars)
            || !Widen(VersionString, Version, GMaxWideChars))
        {
            LOG_WARN("Crash reporting disabled: BUGSPLAT_DATABASE is unset or malformed.");
            GInitialized.store(false, std::memory_order_release);
            return;
        }

        // CrashHandler::Install captures this filter as its previous one, so the local dump lands first.
        if (BugSplat_Init(Database, L"Lumina", Version) == 0)
        {
            LOG_WARN("Crash reporting disabled: BugSplat_Init failed.");
            GInitialized.store(false, std::memory_order_release);
            return;
        }

        GSenderReady.store(true, std::memory_order_release);

        // The SDK asks before sending each one, so this is not a silent flush.
        BugSplat_PostAllCrashesAsync();

        // Shader compiles and cooks block for minutes, and the engine's own watchdog covers real hangs.
        BugSplat_SetHangDetectionTimeout(0);

        // AllocGuardMemory has no C entry point, so a heap-exhaustion crash may fail to report.

        // That helper replaces terminate and purecall handlers wholesale, displacing the ones installed here.

        // Initialize runs before logging exists, so LogStatus reports it once there is a log to report into.
        GStatusLine = FString("BugSplat, version ") + VersionString;
        #endif
    }


    void Shutdown()
    {
        GInitialized.store(false, std::memory_order_release);
    }


    bool IsEnabled()
    {
        #if WITH_BUGSPLAT
        return GInitialized.load(std::memory_order_acquire) && GSenderReady.load(std::memory_order_acquire);
        #else
        return false;
        #endif
    }


    void LogStatus()
    {
        if (IsEnabled())
        {
            LOG_DISPLAY("Crash reporting active ({}).", GStatusLine.c_str());
        }
        else
        {
            // Says WHY, since no reports arriving is otherwise indistinguishable from a broken uploader.
            #if WITH_BUGSPLAT
            LOG_WARN("Crash reporting is compiled in but inactive; crashes will not be uploaded.");
            #else
            LOG_DISPLAY("Crash reporting is not compiled into this build (WITH_BUGSPLAT off).");
            #endif
        }
    }


    void GenerateReport(void* ExceptionPointers)
    {
        #if WITH_BUGSPLAT
        if (!GSenderReady.load(std::memory_order_acquire) || ExceptionPointers == nullptr)
        {
            return;
        }

        // A negative dump type means SDK default, keeping this consistent with the monitor's own reports.
        BugSplat_GenerateDump(ExceptionPointers, -1);
        #else
        (void)ExceptionPointers;
        #endif
    }


    void SetAttribute(FStringView Key, FStringView Value)
    {
        #if WITH_BUGSPLAT
        if (!GSenderReady.load(std::memory_order_acquire))
        {
            return;
        }

        wchar_t WideKey[GMaxWideChars];
        wchar_t WideValue[GMaxWideChars];

        if (Widen(Key, WideKey, GMaxWideChars) && Widen(Value, WideValue, GMaxWideChars))
        {
            BugSplat_SetAttribute(WideKey, WideValue);
        }
        #else
        (void)Key;
        (void)Value;
        #endif
    }


    void AddAttachment(FStringView Path)
    {
        #if WITH_BUGSPLAT
        if (!GSenderReady.load(std::memory_order_acquire))
        {
            return;
        }

        wchar_t WidePath[GMaxWideChars];
        if (!Widen(Path, WidePath, GMaxWideChars))
        {
            return;
        }

        FScopeLock Lock(GAttachmentMutex);

        // The SDK reads the file when a report is built, so it only has to exist at crash time.
        if (BugSplat_AddAttachment(WidePath) != 0)
        {
            GAttachments.emplace_back(WidePath);
        }
        #else
        (void)Path;
        #endif
    }


    void ClearAttachments()
    {
        #if WITH_BUGSPLAT
        if (!GSenderReady.load(std::memory_order_acquire))
        {
            return;
        }

        FScopeLock Lock(GAttachmentMutex);

        // One at a time, since the C API exposes RemoveAttachment but not ClearAttachments.
        for (const FWString& Attachment : GAttachments)
        {
            BugSplat_RemoveAttachment(Attachment.c_str());
        }

        GAttachments.clear();
        #endif
    }


    void SetUser(FStringView Name, FStringView Email)
    {
        #if WITH_BUGSPLAT
        if (!GSenderReady.load(std::memory_order_acquire))
        {
            return;
        }

        wchar_t Wide[GMaxWideChars];

        if (Widen(Name, Wide, GMaxWideChars))
        {
            BugSplat_SetUser(Wide);
        }
        if (Widen(Email, Wide, GMaxWideChars))
        {
            BugSplat_SetEmail(Wide);
        }
        #else
        (void)Name;
        (void)Email;
        #endif
    }
}

#endif
