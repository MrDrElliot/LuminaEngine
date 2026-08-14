#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include "Thread.h"

#include <cstring>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <tracy/Tracy.hpp>

namespace Lumina::Threading
{
    namespace
    {
        constexpr size_t kMaxThreadNameLength = 15;
    }

    uint64 GetThreadID()
    {
        static thread_local const uint64 CachedID = []
        {
        #if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
            return static_cast<uint64>(::gettid());
        #else
            return static_cast<uint64>(::syscall(SYS_gettid));
        #endif
        }();

        return CachedID;
    }

    bool SetThreadName(const char* Name)
    {
        return SetThreadName(Name, ThreadGroup_Other);
    }

    bool SetThreadName(const char* Name, int32 GroupHint)
    {
        if (Name == nullptr)
        {
            return false;
        }

    #ifdef TRACY_ENABLE
        tracy::SetThreadNameWithHint(Name, GroupHint);
    #endif

        char Truncated[kMaxThreadNameLength + 1];
        const size_t Length = ::strnlen(Name, kMaxThreadNameLength);

        ::memcpy(Truncated, Name, Length);
        Truncated[Length] = '\0';

        return ::pthread_setname_np(::pthread_self(), Truncated) == 0;
    }

    bool SetThreadPerformanceHint()
    {
        return false;
    }
}

#endif
