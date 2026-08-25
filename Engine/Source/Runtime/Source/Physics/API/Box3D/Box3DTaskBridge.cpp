#include "RuntimePCH.h"
#include "Box3DTaskBridge.h"

#include "Core/Console/ConsoleVariable.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"

namespace Lumina::Physics
{
    static TConsoleVar CVarPhysicsWorkerCount("Physics.WorkerCount", 0,
        "Box3D solver worker count. 0 auto-picks the physical cores of one cache group, which is what Box3D wants.");

    namespace
    {
        // A step splits into workerCount tasks that spin on each other, so oversubscription costs throughput.
        uint32 ResolvePhysicsWorkerCount()
        {
            const int32 Override = CVarPhysicsWorkerCount.GetValue();
            if (Override > 0)
            {
                return Math::Clamp((uint32)Override, 1u, (uint32)B3_MAX_WORKERS);
            }

            const uint32 LogicalCount = Threading::GetNumThreads();

            TFixedVector<Threading::FCpuTopology, 256> Topology;
            Topology.resize(Math::Min(LogicalCount, 256u));

            const uint32 Described = Threading::GetCpuTopology(Topology.data(), (uint32)Topology.size());
            if (Described == 0)
            {
                // No topology means every processor is equidistant, so half the logical count approximates cores.
                return Math::Clamp(LogicalCount / 2u, 1u, (uint32)B3_MAX_WORKERS);
            }

            uint32 GroupSizes[64] = {};
            uint32 MaxGroup = 0;
            for (uint32 i = 0; i < Described; ++i)
            {
                const uint16 Group = Topology[i].CacheGroup;
                if (Group < 64)
                {
                    ++GroupSizes[Group];
                    MaxGroup = GroupSizes[Group] > GroupSizes[MaxGroup] ? Group : MaxGroup;
                }
            }

            // Distinct physical cores inside the largest cache group, so SMT siblings count once.
            uint64 SeenCores = 0;
            uint32 PhysicalCores = 0;
            for (uint32 i = 0; i < Described; ++i)
            {
                if (Topology[i].CacheGroup != MaxGroup)
                {
                    continue;
                }

                const uint16 Core = Topology[i].PhysicalCore;
                if (Core < 64 && (SeenCores & (1ull << Core)) == 0)
                {
                    SeenCores |= (1ull << Core);
                    ++PhysicalCores;
                }
            }

            // One core is left for the thread driving the step, which enters the solver as worker 0 anyway.
            return Math::Clamp(PhysicalCores > 1 ? PhysicalCores - 1 : 1, 1u, (uint32)B3_MAX_WORKERS);
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
        Jobs::WaitForCounter(Slot->Counter);
        Slot->bInUse.store(false, std::memory_order_release);
    }
}
