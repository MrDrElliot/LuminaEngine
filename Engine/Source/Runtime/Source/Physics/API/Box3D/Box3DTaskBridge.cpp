#include "RuntimePCH.h"
#include "Box3DTaskBridge.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"

namespace Lumina::Physics
{
    namespace
    {
        uint32 ResolvePhysicsWorkerCount()
        {
            const uint32 LogicalCount = Threading::GetNumThreads();
            return Math::Clamp(LogicalCount > 1 ? LogicalCount - 1 : 1, 1u, (uint32)B3_MAX_WORKERS);
        }
    }

    FBox3DTaskBridge::FBox3DTaskBridge()
    {
        WorkerCount = ResolvePhysicsWorkerCount();

        for (FTaskSlot& Slot : Slots)
        {
            Slot.Counter = Jobs::AllocCounter(0);
        }
    }

    FBox3DTaskBridge::~FBox3DTaskBridge()
    {
        for (FTaskSlot& Slot : Slots)
        {
            Jobs::FreeCounter(Slot.Counter);
            Slot.Counter = nullptr;
        }
    }

    void FBox3DTaskBridge::RunTask(void* Arg, uint32 /*WorkerIndex*/)
    {
        LUMINA_PROFILE_SCOPE();
        FTaskSlot* Slot = static_cast<FTaskSlot*>(Arg);
        Slot->Task(Slot->Context);
    }

    void* FBox3DTaskBridge::EnqueueTask(b3TaskCallback* Task, void* TaskContext, void* UserContext, const char* Name)
    {
        FBox3DTaskBridge* Bridge = static_cast<FBox3DTaskBridge*>(UserContext);

        const uint32 Start = Bridge->NextSlotHint.fetch_add(1, std::memory_order_relaxed);
        for (uint32 i = 0; i < B3_MAX_TASKS; ++i)
        {
            FTaskSlot& Slot = Bridge->Slots[(Start + i) % B3_MAX_TASKS];

            bool bExpected = false;
            if (!Slot.bInUse.compare_exchange_strong(bExpected, true, std::memory_order_acquire))
            {
                continue;
            }

            Slot.Task = Task;
            Slot.Context = TaskContext;
            Jobs::RunJob(&RunTask, &Slot, Jobs::EJobPriority::High, Slot.Counter, Name);
            return &Slot;
        }

        // Returning null tells Box3D the work already ran, so it skips the matching FinishTask.
        Task(TaskContext);
        return nullptr;
    }

    void FBox3DTaskBridge::FinishTask(void* UserTask, void* /*UserContext*/)
    {
        FTaskSlot* Slot = static_cast<FTaskSlot*>(UserTask);

        // Box3D's tasks spin on each other inside one step, so this wait is microseconds and parking loses.
        Jobs::WaitForCounterBusy(Slot->Counter);
        Slot->bInUse.store(false, std::memory_order_release);
    }
}
