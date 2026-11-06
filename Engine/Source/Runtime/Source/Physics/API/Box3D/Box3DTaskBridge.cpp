#include "RuntimePCH.h"
#include "Box3DTaskBridge.h"
#include "Containers/HashTable.h"
#include "Core/Console/ConsoleVariable.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"

namespace Lumina::Physics
{
    namespace Box3DTaskBridgeInternal
    {
        TConsoleVar<int32> CVarBox3DWorkers("Physics.Box3D.Workers", 0,
            "Workers each Box3D world steps with, read when a world is created. Zero picks one per physical core.");

        uint32 CountPhysicalCores()
        {
            constexpr uint32 MaxDescribed = 256;
            Threading::FCpuTopology Topology[MaxDescribed];
            const uint32 Described = Threading::GetCpuTopology(Topology, MaxDescribed);
            if (Described == 0)
            {
                // Without topology, assume two hardware threads per core, which is the common case.
                return Math::Max(Threading::GetNumThreads() / 2u, 1u);
            }

            THashSet<uint16> Cores;
            for (uint32 Index = 0; Index < Described; ++Index)
            {
                Cores.Add(Topology[Index].PhysicalCore);
            }
            return (uint32)Cores.Num();
        }

        // One per physical core, capped at the pool plus the stepping thread, since the solver's workers spin on each other.
        uint32 ResolvePhysicsWorkerCount()
        {
            const int32  Requested = CVarBox3DWorkers.GetValue();
            const uint32 Wanted    = Requested > 0 ? (uint32)Requested : CountPhysicalCores();
            const uint32 Limit     = Math::Min((uint32)B3_MAX_WORKERS, Jobs::GetNumWorkers() + 1u);
            return Math::Clamp(Wanted, 1u, Limit);
        }
    }

    using namespace Box3DTaskBridgeInternal;

    FBox3DTaskBridge::FBox3DTaskBridge()
    {
        WorkerCount = ResolvePhysicsWorkerCount();
    }

    FBox3DTaskBridge::~FBox3DTaskBridge() = default;

    Jobs::TTask<void> FBox3DTaskBridge::RunTask(FTaskSlot* Slot)
    {
        LUMINA_PROFILE_SCOPE();
        Slot->Task(Slot->Context);
        co_return;
    }

    void* FBox3DTaskBridge::EnqueueTask(b3TaskCallback* Task, void* TaskContext, void* UserContext, const char*)
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
            Slot.Done.Reset();
            Jobs::Spawn(Jobs::EJobPriority::High, Slot.Done, &RunTask, &Slot);
            return &Slot;
        }

        // Returning null tells Box3D the work already ran, so it skips the matching FinishTask.
        Task(TaskContext);
        return nullptr;
    }

    void FBox3DTaskBridge::FinishTask(void* UserTask, void* /*UserContext*/)
    {
        FTaskSlot* Slot = static_cast<FTaskSlot*>(UserTask);

        Slot->Done.Wait();
        Slot->bInUse.store(false, std::memory_order_release);
    }
}
