#include "RuntimePCH.h"
#include "Task.h"

#include "Memory/Memory.h"

namespace Lumina::CoroDetail
{
    namespace
    {
        void ResumeThunk(void* Arg, uint32 /*Worker*/)
        {
            std::coroutine_handle<>::from_address(Arg).resume();
        }
    }

    void ScheduleResume(std::coroutine_handle<> Handle, ETaskPriority Priority)
    {
        Jobs::RunJob(&ResumeThunk, Handle.address(), ToJobPriority(Priority), nullptr, "Coro");
    }

    struct FDetachedRoot
    {
        std::coroutine_handle<> Handle;
        Jobs::FCounter*         Counter = nullptr;

        static void OnDone(void* Ctx, uint32 /*Worker*/)
        {
            FDetachedRoot* Self = static_cast<FDetachedRoot*>(Ctx);
            Self->Handle.destroy();
            Jobs::FreeCounter(Self->Counter);
            Memory::Delete(Self);
        }
    };

    void LaunchDetached(std::coroutine_handle<> Handle, FPromiseBase& Promise, ETaskPriority Priority)
    {
        FDetachedRoot* Root = Memory::New<FDetachedRoot>();
        Root->Handle  = Handle;
        Root->Counter = Jobs::AllocCounter(1);

        Promise.CompletionCounter = Root->Counter;
        Promise.Priority          = Priority;

        Jobs::SetCounterCompletion(Root->Counter, &FDetachedRoot::OnDone, Root);
        ScheduleResume(Handle, Priority);
    }
}
