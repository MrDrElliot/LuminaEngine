#include "RuntimePCH.h"
#include "AnimNotifyQueue.h"

#include "AnimNotify.h"
#include "Containers/Algorithm.h"
#include "TaskSystem/Scheduler/JobScheduler.h"
#include "World/ECS/CommandBus.h"
#include "World/ECS/Registry.h"

namespace Lumina
{
    void FAnimNotifyQueue::Prepare()
    {
        const uint32 NumSlots = Math::Max(Jobs::GetNumThreadSlots(), 1u);
        if (Slots.size() < NumSlots)
        {
            Slots.resize(NumSlots);
        }
    }

    void FAnimNotifyQueue::Record(ECS::FEntity Entity, EAnimNotifyPass Pass, const TVector<FAnimNotifyEvent>& Events)
    {
        const uint32 SlotIndex = Jobs::GetWorkerIndex();
        DEBUG_ASSERT(SlotIndex < Slots.size(), "FAnimNotifyQueue::Prepare must run before the passes record");
        TVector<FRecord>& Slot = Slots[SlotIndex];

        for (const FAnimNotifyEvent& Event : Events)
        {
            if (Event.Notify == nullptr && Event.State == nullptr)
            {
                continue;
            }

            FRecord& Record = Slot.emplace_back();
            Record.Source = TStrongObjectPtr<const CObject>(Event.Source);
            Record.Notify = Event.Notify;
            Record.State  = Event.State;
            Record.Entity = Entity;
            Record.Alpha  = Event.Alpha;
            Record.Type   = Event.Type;
            Record.Pass   = Pass;
        }
    }

    void FAnimNotifyQueue::Submit(ECS::FCommandBus& Bus)
    {
        SIZE_T Count = 0;
        for (const TVector<FRecord>& Slot : Slots)
        {
            Count += Slot.size();
        }
        if (Count == 0)
        {
            return;
        }

        TVector<FRecord> Records;
        Records.reserve(Count);
        for (TVector<FRecord>& Slot : Slots)
        {
            for (FRecord& Record : Slot)
            {
                Records.push_back(std::move(Record));
            }
            Slot.clear();
        }

        // Workers claim entities in any order, and a stable sort restores one order without reordering an entity's own events.
        Algo::StableSort(Records, [](const FRecord& A, const FRecord& B)
        {
            return A.Pass != B.Pass ? A.Pass < B.Pass : A.Entity.Value < B.Entity.Value;
        });

        Bus.Enqueue([Records = std::move(Records)](ECS::FRegistry& Registry)
        {
            Dispatch(Records, Registry);
        });
    }

    void FAnimNotifyQueue::Dispatch(const TVector<FRecord>& Records, ECS::FRegistry& Registry)
    {
        LUMINA_PROFILE_SCOPE();

        for (const FRecord& Record : Records)
        {
            // An earlier handler may have destroyed the entity.
            if (!Registry.IsValid(Record.Entity))
            {
                continue;
            }

            switch (Record.Type)
            {
            case EAnimNotifyEventType::Trigger:
                if (Record.Notify != nullptr)
                {
                    Record.Notify->Notify(Registry, Record.Entity);
                }
                break;
            case EAnimNotifyEventType::Begin:
                if (Record.State != nullptr)
                {
                    Record.State->NotifyBegin(Registry, Record.Entity);
                }
                break;
            case EAnimNotifyEventType::Tick:
                if (Record.State != nullptr)
                {
                    Record.State->NotifyTick(Registry, Record.Entity, Record.Alpha);
                }
                break;
            case EAnimNotifyEventType::End:
                if (Record.State != nullptr)
                {
                    Record.State->NotifyEnd(Registry, Record.Entity);
                }
                break;
            }
        }
    }
}
