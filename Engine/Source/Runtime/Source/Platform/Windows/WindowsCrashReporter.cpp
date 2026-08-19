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

// The flat C API, not the BugSplat class in BugSplat.h. Only the C ABI is exported from
// BugSplat.dll -- the C++ class lives in the static libs, which would pin this module to one CRT
// and force a per-configuration lib for /MD, /MDd, /MT and /MTd. The C entry points are
// CRT-agnostic, so one 158KB DLL serves every configuration.
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

        // Widens into Out, returning false if it does not fit. A truncated path attaches the wrong
        // file and a truncated attribute is a lie, so anything oversized is dropped rather than cut.
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

        // The C API owns a single process-wide instance, so there is no handle to hold. It is never
        // torn down: it owns the unhandled-exception filter for the life of the process, and a crash
        // during static teardown must not land on a destroyed one.
        std::atomic<bool> GSenderReady{ false };

        // The C API has RemoveAttachment but no ClearAttachments, so the paths handed to it are
        // tracked here to be removed one by one. Touched only when a project loads, never from the
        // crash path.
        FMutex          GAttachmentMutex;
        TVector<FWString>   GAttachments;

        // Short commit hash of the tree this binary was built from, or empty.
        //
        // Read from .git at runtime rather than baked in by the build: everyone running the editor
        // built it themselves, from whatever commit they happened to be on, so without this every
        // report in the dashboard is filed under one meaningless version. Runtime keeps it out of
        // the compile, which would otherwise rebuild the world on every commit.
        FString ReadGitCommit()
        {
            auto ParentOf = [](FStringView Path) -> FString
            {
                const size_t Slash = Path.find_last_of("/\\");
                return Slash == FStringView::npos ? FString() : FString(Path.data(), Slash);
            };

            // Exe lives at <root>/Binaries/<Platform>/, matching Paths' own exe-relative fallback.
            // Paths is not initialized this early, so the walk is repeated here rather than shared.
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
                    // No loose ref. git packs refs during gc and a fresh clone may never write one,
                    // so this is the normal state on a user's machine rather than an edge case --
                    // without the fallback every report from them loses its commit.
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

            // Short hash: enough to identify the commit, short enough to read in a version column.
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
        // Version is what the dashboard groups by, so the commit goes in it: an editor built from
        // source is only identifiable by the tree it came from. For a packaged build with no .git
        // the commit is empty and this falls back to version-configuration, which is also the form
        // UploadSymbols.ps1 pushes symbols under.
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

        // Installs the unhandled-exception filter. CrashHandler::Install runs straight after this
        // and captures that filter as its previous one, which is what lets the local dump be written
        // first and then handed off.
        if (BugSplat_Init(Database, L"Lumina", Version) == 0)
        {
            LOG_WARN("Crash reporting disabled: BugSplat_Init failed.");
            GInitialized.store(false, std::memory_order_release);
            return;
        }

        GSenderReady.store(true, std::memory_order_release);

        // Drain anything queued by an earlier session that could not upload -- offline, or the user
        // dismissed the sender. The SDK asks before sending each one, so this is not a silent flush.
        BugSplat_PostAllCrashesAsync();

        // The engine blocks well past five seconds on ordinary work -- shader compiles, cooks, asset
        // imports -- and BugSplat's hang detector would report every one of those as a crash. The
        // engine's own HangWatchdog already covers real hangs with context this has no way to know.
        BugSplat_SetHangDetectionTimeout(0);

        // No guard-memory reservation: AllocGuardMemory is on the C++ class and has no C entry
        // point, so a crash caused by heap exhaustion may fail to report. The cost of getting it
        // back is linking the static lib per CRT variant, which is not worth it for that one case.

        // SetGlobalCRTExceptionBehavior is intentionally not called either -- it is an inline helper
        // in BugSplat.h that replaces set_terminate, the purecall handler and the invalid-parameter
        // handler wholesale, silently displacing the ones CrashHandler::Install registered. Those
        // route terminate and SIGABRT through the same filter, so the path is covered already.

        // Stored rather than logged. Initialize() has to run before CrashHandler::Install(), which is
        // before FApplicationGlobalState brings logging up, so a LOG_ here reaches nothing -- which is
        // why no log in the wild has ever shown whether the reporter started. LogStatus() reports it
        // once there is a log to report into.
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
            // Says WHY, because "no reports are arriving" is otherwise indistinguishable from the
            // uploader being broken.
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

        // Negative dumpType means "SDK default", which keeps this consistent with the reports the
        // monitor produces on its own rather than inventing a second minidump shape.
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

        // The SDK holds the path and reads the file when a report is built, so the file only has to
        // exist at crash time, not now.
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

        // One at a time: the C API exposes RemoveAttachment but not ClearAttachments.
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
