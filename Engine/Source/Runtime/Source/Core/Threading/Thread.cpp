#include "RuntimePCH.h"
#include "Thread.h"
#include "Platform/Time/PlatformTime.h"
#include "rpmalloc.h"
#include "Core/Assertions/Assert.h"


namespace Lumina
{
    namespace Threading
    {
        static uint64 GMainThreadID = 0;

        void ThreadYield()
        {
            PlatformTime::YieldThread();
        }


        bool IsMainThread()
        {
            return GMainThreadID != 0 && GMainThreadID == GetThreadID();
        }

        void Sleep(uint64 Milliseconds)
        {
            PlatformTime::SleepMilliseconds(static_cast<uint32>(Milliseconds));
        }

        void Initialize(const char* MainThreadName)
        {
            GMainThreadID = GetThreadID();
            SetThreadName(MainThreadName, ThreadGroup_Main);
        }

        void Shutdown()
        {
            GMainThreadID = 0;
        }

        void InitializeThreadHeap()
        {
            rpmalloc_thread_initialize();
        }

        void ShutdownThreadHeap()
        {
            rpmalloc_thread_finalize(1);
        }

    }
}
