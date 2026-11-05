#include "RuntimePCH.h"
#ifdef LE_PLATFORM_LINUX

#include "Thread.h"

#include <cstring>
#include <pthread.h>
#include <linux/futex.h>
#include <linux/membarrier.h>
#include <sys/resource.h>
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

    bool SetCurrentThreadPriority(EThreadPriority Priority)
    {
        const int Nice = Priority == EThreadPriority::Background ? 10 : Priority == EThreadPriority::TimeCritical ? -10 : 0;
        return ::setpriority(PRIO_PROCESS, (id_t)GetThreadID(), Nice) == 0;
    }

    bool WaitOnAddress32(const volatile uint32* Address, uint32 Compare, uint32 TimeoutMs)
    {
        timespec Timeout;
        Timeout.tv_sec  = (time_t)(TimeoutMs / 1000u);
        Timeout.tv_nsec = (long)((TimeoutMs % 1000u) * 1000000u);
        const long Result = ::syscall(SYS_futex, (uint32*)Address, FUTEX_WAIT_PRIVATE, Compare, &Timeout, nullptr, 0);
        return Result == 0;
    }

    void WakeAllOnAddress32(const volatile uint32* Address)
    {
        ::syscall(SYS_futex, (uint32*)Address, FUTEX_WAKE_PRIVATE, INT32_MAX, nullptr, nullptr, 0);
    }

    void WakeOneOnAddress32(const volatile uint32* Address)
    {
        ::syscall(SYS_futex, (uint32*)Address, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }

    bool ProcessWideMemoryBarrier()
    {
        static const bool bRegistered = ::syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
        if (bRegistered && ::syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) == 0)
        {
            return true;
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return false;
    }

    // Without it the scheduler treats every worker as equidistant, as it did before topology existed.
    uint32 GetCpuTopology(FCpuTopology* /*Out*/, uint32 /*MaxCount*/)
    {
        return 0;
    }

    void SetThreadIdealProcessor(uint32 /*LogicalProcessor*/)
    {
    }
}


namespace Lumina
{
    namespace
    {
        // Copied so the name outlives a caller that built it on the stack.
        struct FThreadStart
        {
            char                        Name[64] = {};
            int32                       Group = Threading::ThreadGroup_Other;
            EThreadPriority             Priority = EThreadPriority::Normal;
            TMoveOnlyFunction<void()>*  Body = nullptr;
            std::atomic<uint32>         KernelId{ 0 };
            std::atomic<uint32>         References{ 2 };

            FThreadStart(const FThreadOptions& Options, TMoveOnlyFunction<void()>* InBody)
                : Group(Options.Group), Priority(Options.Priority), Body(InBody)
            {
                if (Options.Name != nullptr)
                {
                    for (size_t i = 0; i + 1 < sizeof(Name) && Options.Name[i] != 0; ++i)
                    {
                        Name[i] = Options.Name[i];
                    }
                }
            }

            void Run()
            {
                if (Name[0] != 0)
                {
                    Threading::SetThreadName(Name, Group);
                }
                if (Priority != EThreadPriority::Normal)
                {
                    Threading::SetCurrentThreadPriority(Priority);
                }
                (*Body)();
                delete Body;
            }

            void Release()
            {
                if (References.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    delete this;
                }
            }
        };

        void* ThreadEntry(void* Parameter)
        {
            FThreadStart* Start = static_cast<FThreadStart*>(Parameter);
            Start->KernelId.store(static_cast<uint32>(Threading::GetThreadID()), std::memory_order_release);
            Threading::WakeAllOnAddress32(reinterpret_cast<const volatile uint32*>(&Start->KernelId));
            Start->Run();
            Start->Release();
            return nullptr;
        }
    }

    void FThread::Start(const FThreadOptions& Options, TMoveOnlyFunction<void()>* Body)
    {
        FThreadStart* Begin = new FThreadStart(Options, Body);
        pthread_attr_t Attributes;
        pthread_attr_init(&Attributes);
        if (Options.StackSize > 0)
        {
            pthread_attr_setstacksize(&Attributes, Options.StackSize);
        }

        pthread_t Thread{};
        const int Created = pthread_create(&Thread, &Attributes, &ThreadEntry, Begin);
        pthread_attr_destroy(&Attributes);
        if (Created != 0)
        {
            delete Body;
            delete Begin;
            return;
        }

        uint32 KernelId = Begin->KernelId.load(std::memory_order_acquire);
        while (KernelId == 0)
        {
            Threading::WaitOnAddress32(reinterpret_cast<const volatile uint32*>(&Begin->KernelId), 0, 100);
            KernelId = Begin->KernelId.load(std::memory_order_acquire);
        }
        Begin->Release();

        Handle = reinterpret_cast<void*>(Thread);
        Id = static_cast<uint64>(KernelId);
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
            Stop = std::move(Other.Stop);
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
