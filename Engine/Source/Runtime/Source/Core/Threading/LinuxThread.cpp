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

    uint32 GetNumThreads()
    {
        const long Count = ::sysconf(_SC_NPROCESSORS_ONLN);
        return Count > 0 ? static_cast<uint32>(Count) : 1u;
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


namespace Lumina
{
    namespace
    {
        void* ThreadEntry(void* Parameter)
        {
            TMoveOnlyFunction<void()>* Body = static_cast<TMoveOnlyFunction<void()>*>(Parameter);
            (*Body)();
            delete Body;
            return nullptr;
        }
    }

    void FThread::Start(TMoveOnlyFunction<void()>* Body)
    {
        pthread_t Thread{};
        if (pthread_create(&Thread, nullptr, &ThreadEntry, Body) != 0)
        {
            delete Body;
            return;
        }

        Handle = reinterpret_cast<void*>(Thread);
        Id = static_cast<uint64>(Thread);
    }

    void FThread::Join() noexcept
    {
        if (Handle == nullptr)
        {
            return;
        }

        pthread_join(reinterpret_cast<pthread_t>(Handle), nullptr);
        Handle = nullptr;
        Id = 0;
    }

    void FThread::Detach() noexcept
    {
        if (Handle == nullptr)
        {
            return;
        }

        pthread_detach(reinterpret_cast<pthread_t>(Handle));
        Handle = nullptr;
        Id = 0;
    }

    FThread& FThread::operator=(FThread&& Other) noexcept
    {
        if (this != &Other)
        {
            Detach();
            Handle = Other.Handle;
            Id = Other.Id;
            Other.Handle = nullptr;
            Other.Id = 0;
        }

        return *this;
    }

    FThread::~FThread()
    {
        Detach();
    }
}

#endif
