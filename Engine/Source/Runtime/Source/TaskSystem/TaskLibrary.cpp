#include "RuntimePCH.h"

#include "TaskLibrary.h"

#include "TaskSystem/TaskSystem.h"

namespace Lumina
{
    namespace TaskLibraryInternal
    {
        Jobs::EJobPriority ToBand(int32 Priority)
        {
            return static_cast<Jobs::EJobPriority>(std::clamp<int32>(Priority, 0, static_cast<int32>(Jobs::EJobPriority::Background)));
        }

        struct FScriptTaskState
        {
            std::atomic<uint32> Refs{2};
            Jobs::FTaskEvent    Done;

            void Release()
            {
                if (Refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    delete this;
                }
            }
        };

        FScriptTaskState* ToState(uint64 Handle)
        {
            return reinterpret_cast<FScriptTaskState*>(static_cast<uintptr_t>(Handle));
        }

        Jobs::TTask<void> RunScriptTask(SScriptCallback Body, FScriptTaskState* State)
        {
            Scripting::InvokeScriptCallback(Body, 0);
            State->Done.Signal();
            State->Release();
            co_return;
        }
    }

    using namespace TaskLibraryInternal;

    uint64 CTaskLibrary::Run(SScriptCallback Body, int32 Priority)
    {
        if (!Body.IsBound())
        {
            return 0;
        }

        FScriptTaskState* State = new FScriptTaskState();
        Jobs::Spawn(ToBand(Priority), &RunScriptTask, Body, State);
        return static_cast<uint64>(reinterpret_cast<uintptr_t>(State));
    }

    void CTaskLibrary::ParallelFor(uint32 Num, uint32 MinRange, SScriptCallback Body, int32 Priority)
    {
        if (Body.IsBound() && Num > 0)
        {
            Jobs::ParallelFor(Num, [Body](const Jobs::FParallelRange& Range)
            {
                Scripting::InvokeScriptCallbackRepeating(Body, static_cast<uint64>(Range.Start) | (static_cast<uint64>(Range.End) << 32));
            }, MinRange, ToBand(Priority));
        }
        Scripting::ReleaseScriptCallback(Body);
    }

    void CTaskLibrary::Wait(uint64 Handle)
    {
        if (Handle != 0)
        {
            ToState(Handle)->Done.Wait();
        }
    }

    void CTaskLibrary::Release(uint64 Handle)
    {
        if (Handle != 0)
        {
            ToState(Handle)->Release();
        }
    }

    void CTaskLibrary::WaitForAll()
    {
        Jobs::WaitForSpawned();
    }

    int32 CTaskLibrary::NumWorkers()
    {
        return static_cast<int32>(Jobs::GetNumWorkers());
    }
}
