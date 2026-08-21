#include "RuntimePCH.h"
#include "Fiber.h"

#ifdef LE_PLATFORM_WINDOWS

#include "Memory/Memory.h"
#include "Core/Assertions/Assert.h"
#include <windows.h>

namespace Lumina::Fibers
{
    namespace
    {
        // Freed on first entry, since the real entry never returns through here.
        struct FFiberStart
        {
            FFiberEntry Entry;
            void*       Arg;
        };

        void WINAPI FiberTrampoline(void* Param)
        {
            FFiberStart Start = *static_cast<FFiberStart*>(Param);
            void* Mem = Param;
            Memory::Free(Mem);
            Start.Entry(Start.Arg);
        }

        // Commit a small slice up front; Windows grows committed pages on demand up to the reserve.
        constexpr size_t kStackCommit = 32 * 1024;
    }

    FFiber ThreadToFiber()
    {
        // The float-switch flag saves x87 and MXCSR, or FP control state leaks between fibers.
        if (::IsThreadAFiber())
        {
            return ::GetCurrentFiber();
        }
        FFiber Fiber = ::ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        ASSERT(Fiber != nullptr);
        return Fiber;
    }

    void FiberToThread()
    {
        if (::IsThreadAFiber())
        {
            ::ConvertFiberToThread();
        }
    }

    FFiber Create(size_t StackSize, FFiberEntry Entry, void* Arg)
    {
        FFiberStart* Start = static_cast<FFiberStart*>(Memory::Malloc(sizeof(FFiberStart), alignof(FFiberStart)));
        Start->Entry = Entry;
        Start->Arg   = Arg;

        const SIZE_T Commit = static_cast<SIZE_T>(kStackCommit < StackSize ? kStackCommit : StackSize);
        FFiber Fiber = ::CreateFiberEx(Commit, static_cast<SIZE_T>(StackSize), FIBER_FLAG_FLOAT_SWITCH, &FiberTrampoline, Start);
        ASSERT(Fiber != nullptr);
        return Fiber;
    }

    void Destroy(FFiber Fiber)
    {
        if (Fiber != nullptr)
        {
            ::DeleteFiber(Fiber);
        }
    }

    void Switch(FFiber Fiber)
    {
        ::SwitchToFiber(Fiber);
    }

    FFiber Current()
    {
        return ::IsThreadAFiber() ? ::GetCurrentFiber() : nullptr;
    }
}

#endif
