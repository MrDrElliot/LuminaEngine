#include "RuntimePCH.h"
#include "Thread.h"
#include "rpmalloc.h"
#include "Core/Assertions/Assert.h"


namespace Lumina
{
    namespace Threading
    {
        static std::thread::id GMainThreadID = {};
        static std::thread::id GRenderThreadID = {};
        static std::thread::id GPhysicsThreadID = {};

        void ThreadYield()
        {
            std::this_thread::yield();
        }

        uint64 GetThreadID()
        {
            return std::this_thread::get_id()._Get_underlying_id();
        }

        bool IsMainThread()
        {
            return GMainThreadID == std::this_thread::get_id();
        }

        bool IsRenderThread()
        {
            // Before SetRenderThread runs (boot / shutdown), the main thread is the render thread.
            const std::thread::id Render = GRenderThreadID;
            return Render == std::thread::id{}
                ? GMainThreadID == std::this_thread::get_id()
                : Render == std::this_thread::get_id();
        }

        bool IsPhysicsThread()
        {
            // Before SetPhysicsThread runs (boot / shutdown), the main thread is the physics thread.
            const std::thread::id Physics = GPhysicsThreadID;
            return Physics == std::thread::id{}
                ? GMainThreadID == std::this_thread::get_id()
                : Physics == std::this_thread::get_id();
        }

        void SetRenderThread(std::thread::id ID)
        {
            GRenderThreadID = ID;
        }

        void SetPhysicsThread(std::thread::id ID)
        {
            GPhysicsThreadID = ID;
        }

        uint32 GetNumThreads()
        {
            return std::thread::hardware_concurrency();
        }

        void Sleep(uint64 Milliseconds)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(Milliseconds));
        }

        void Initialize(const char* MainThreadName)
        {
            GMainThreadID = std::this_thread::get_id();
            SetThreadName(MainThreadName, ThreadGroup_Main);
        }

        void Shutdown()
        {
            GMainThreadID = {};
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
