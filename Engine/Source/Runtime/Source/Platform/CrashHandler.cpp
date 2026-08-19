#include "Core/Threading/Thread.h"
#include "RuntimePCH.h"
#include "Platform/CrashHandler.h"

#include <atomic>
#include <cstring>

#include "Paths/Paths.h"
#include "Platform/Process/PlatformProcess.h"

namespace Lumina::CrashHandler
{
    namespace
    {
        constexpr uint32 GMaxDirectoryChars = 512;

        // Plain storage rather than an FString: the crash path reads this after the heap may already be
        // corrupt. Double buffered so a repoint mid-crash hands the reader one whole path or the other,
        // never a half-overwritten one.
        char                GDirectories[2][GMaxDirectoryChars] = {};
        std::atomic<uint32> GActive{ 0 };
        std::atomic<uint32> GLengths[2] = {};

        FMutex GPublishMutex;

        void Publish(const char* Chars, uint32 Length)
        {
            if (Length >= GMaxDirectoryChars)
            {
                Length = GMaxDirectoryChars - 1;
            }

            FScopeLock Lock(GPublishMutex);

            const uint32 Next = 1 - GActive.load(std::memory_order_relaxed);
            std::memcpy(GDirectories[Next], Chars, Length);
            GDirectories[Next][Length] = 0;
            GLengths[Next].store(Length, std::memory_order_relaxed);

            GActive.store(Next, std::memory_order_release);
        }

        // Install() warms this so the crash path is never the first caller, which would allocate.
        void EnsureDefault()
        {
            static FOnceFlag Once;
            CallOnce(Once, []
            {
                FString ExePath = Platform::GetCurrentProcessPath();
                Paths::Normalize(ExePath);

                const FString Default = Paths::Parent(ExePath) + "/CrashDumps";
                Publish(Default.c_str(), static_cast<uint32>(Default.size()));
            });
        }
    }


    void SetCrashDumpDirectory(FStringView Directory)
    {
        if (Directory.empty())
        {
            return;
        }

        EnsureDefault();
        Publish(Directory.data(), static_cast<uint32>(Directory.size()));
    }


    FString GetCrashDumpDirectory()
    {
        EnsureDefault();

        const uint32 Active = GActive.load(std::memory_order_acquire);
        return FString(GDirectories[Active], GLengths[Active].load(std::memory_order_relaxed));
    }


    uint32 GetCrashDumpDirectory(char* OutBuffer, uint32 BufferSize)
    {
        if (OutBuffer == nullptr || BufferSize == 0)
        {
            return 0;
        }

        const uint32 Active = GActive.load(std::memory_order_acquire);
        uint32 Length = GLengths[Active].load(std::memory_order_relaxed);
        if (Length >= BufferSize)
        {
            // Truncating a directory would point the dump somewhere else entirely; let the caller fall back.
            OutBuffer[0] = 0;
            return 0;
        }

        std::memcpy(OutBuffer, GDirectories[Active], Length);
        OutBuffer[Length] = 0;
        return Length;
    }
}
