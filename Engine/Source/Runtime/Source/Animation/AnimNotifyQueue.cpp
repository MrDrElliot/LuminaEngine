#include "RuntimePCH.h"
#include "AnimNotifyQueue.h"

#include "AnimNotify.h"
#include "Containers/Algorithm.h"
#include "TaskSystem/Fork/ForkRuntime.h"
#include "World/ECS/CommandBus.h"
#include "World/ECS/Registry.h"
#include "World/Entity/Systems/SystemContext.h"

namespace Lumina
{
    void FAnimNotifyQueue::Prepare()
    {
        const uint32 NumSlots = Math::Max(Jobs::GetNumThreadSlots(), 1u);
        if (static_cast<uint32>(Slots.Num()) < NumSlots)
        {
            Slots.SetNum(NumSlots);
        }
    }

    void FAnimNotifyQueue::Record(ECS::FEntity Entity, EAnimNotifyPass Pass, const TVector<FAnimNotifyEvent>& Events)
    {
        const uint32 SlotIndex = Jobs::GetThreadSlot();
        DEBUG_ASSERT(SlotIndex < static_cast<uint32>(Slots.Num()), "FAnimNotifyQueue::Prepare must run before the passes record");
        TVector<FRecord>& Slot = Slots[SlotIndex];

        for (const FAnimNotifyEvent& Event : Events)
        {
            if (Event.Notify == nullptr && Event.State == nullptr)
            {
                continue;
            }

            FRecord& Record = Slot.EmplaceGetRef();
            Record.Source = TStrongObjectPtr<const CObject>(Event.Source);
            Record.Notify = Event.Notify;
            Record.State  = Event.State;
            Record.Entity = Entity;
            Record.Alpha  = Event.Alpha;
            Record.Type   = Event.Type;
            Record.Pass   = Pass;
        }
    }

    TVector<FAnimNotifyQueue::FRecord> FAnimNotifyQueue::TakeSorted()
    {
        SIZE_T Count = 0;
        for (const TVector<FRecord>& Slot : Slots)
        {
            Count += Slot.Num();
        }

        TVector<FRecord> Records;
        if (Count == 0)
        {
            return Records;
        }

        Records.Reserve(IntCastChecked<int32>(Count));
        for (TVector<FRecord>& Slot : Slots)
        {
            for (FRecord& Record : Slot)
            {
                Records.Add(Move(Record));
            }
            Slot.Reset();
        }

        // Workers claim entities in any order, and a stable sort restores one order without reordering an entity's own events.
        Algo::StableSort(Records, [](const FRecord& A, const FRecord& B)
        {
            return A.Pass != B.Pass ? A.Pass < B.Pass : A.Entity.Value < B.Entity.Value;
        });
        return Records;
    }

    void FAnimNotifyQueue::Submit(ECS::FCommandBus& Bus)
    {
        TVector<FRecord> Records = TakeSorted();
        if (Records.IsEmpty())
        {
            return;
        }

        Bus.Enqueue([Records = Move(Records)](ECS::FRegistry& Registry)
        {
            Dispatch(Records, Registry);
        });
    }

    void FAnimNotifyQueue::Submit(const SSystemContext& Context)
    {
        TVector<FRecord> Records = TakeSorted();
        if (Records.IsEmpty())
        {
            return;
        }

        Context.Defer([Records = Move(Records)](const SExclusiveContext& Exclusive)
        {
            Dispatch(Records, Exclusive.GetRegistry());
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
