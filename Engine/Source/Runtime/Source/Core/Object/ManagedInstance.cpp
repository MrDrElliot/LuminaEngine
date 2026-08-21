#include "RuntimePCH.h"
#include "ManagedInstance.h"

#include "Containers/Vector.h"
#include "Core/Threading/Thread.h"
#include "Log/Log.h"
#include "Lumina.h"
#include "ObjectBase.h"

namespace Lumina
{
    namespace
    {
        // Mirrors CObjectBase's fields MINUS ManagedInstanceSlot, in the same order. The slot was added to sit
        // in the padding between the 4-byte ObjectFlags and the pointer after it, so caching a managed
        // instance should cost no object growth -- if it ever does, every CObject in the engine gets bigger
        // silently. Comparing against a probe rather than a magic number keeps the check meaningful after a
        // field is added: mirror the addition here too, and the assert goes on measuring what it claims to.
        struct FObjectBaseLayoutProbe
        {
            virtual ~FObjectBaseLayoutProbe() = default;

            EObjectFlags Flags;
            CClass*      Class;
            CPackage*    Package;
            FName        Name;
            FGuid        Guid;
            int32        InternalIndex;
            int32        LoaderIndex;
        };
    }

    static_assert(sizeof(CObjectBase) == sizeof(FObjectBaseLayoutProbe),
        "ManagedInstanceSlot grew CObjectBase instead of fitting the padding after ObjectFlags. Re-check the "
        "field order in Core/Object/ObjectBase.h before accepting a larger object.");

    class FManagedInstanceTable
    {
    public:

        static FManagedInstanceTable& Get()
        {
            static FManagedInstanceTable Instance;
            return Instance;
        }

        void SetFreeHandleFn(ManagedInstances::FFreeHandleFn Fn)
        {
            FreeHandleFn = Fn;
        }

        void* Find(const CObjectBase* Object) const
        {
            if (Object == nullptr || Object->ManagedInstanceSlot == INDEX_NONE)
            {
                return nullptr;
            }

            const int32 Slot = Object->ManagedInstanceSlot;
            if (!IsValidSlot(Slot))
            {
                ReportBadSlot("Find", Object, Slot);
                return nullptr;
            }
            return Slots[Slot];
        }

        void Set(CObjectBase* Object, void* Handle)
        {
            if (Object == nullptr)
            {
                return;
            }

            if (Handle == nullptr)
            {
                Release(Object);
                return;
            }

            NoteMutation();

            const int32 Existing = Object->ManagedInstanceSlot;
            if (Existing != INDEX_NONE && !IsValidSlot(Existing))
            {
                ReportBadSlot("Set", Object, Existing);
                Object->ManagedInstanceSlot = INDEX_NONE;
            }

            if (Object->ManagedInstanceSlot != INDEX_NONE)
            {
                // Replacing an instance (the previous wrapper was collected, or was the wrong type).
                FreeHandle(Slots[Object->ManagedInstanceSlot]);
                Slots[Object->ManagedInstanceSlot] = Handle;
                return;
            }

            Object->ManagedInstanceSlot = AcquireSlot();
            Slots[Object->ManagedInstanceSlot] = Handle;
            Owners[Object->ManagedInstanceSlot] = Object;
            ++LiveCount;
        }

        void Release(CObjectBase* Object)
        {
            if (Object == nullptr || Object->ManagedInstanceSlot == INDEX_NONE)
            {
                return;
            }

            NoteMutation();

            const int32 Slot = Object->ManagedInstanceSlot;
            Object->ManagedInstanceSlot = INDEX_NONE;

            // A slot the table never handed out must not reach FreeSlots; it would be reissued as a valid one.
            if (!IsValidSlot(Slot))
            {
                ReportBadSlot("Release", Object, Slot);
                return;
            }

            FreeHandle(Slots[Slot]);
            Slots[Slot] = nullptr;
            Owners[Slot] = nullptr;
            FreeSlots.push_back(Slot);
            --LiveCount;
        }

        void ReleaseAll()
        {
            NoteMutation();

            // The owning objects' slot indices are cleared through the back-reference list, so a later
            // Release/Find on a surviving object sees INDEX_NONE rather than a recycled slot.
            for (int32 Slot = 0; Slot < (int32)Slots.size(); ++Slot)
            {
                if (Owners[Slot] != nullptr)
                {
                    Owners[Slot]->ManagedInstanceSlot = INDEX_NONE;
                    Owners[Slot] = nullptr;
                }
                if (Slots[Slot] != nullptr)
                {
                    FreeHandle(Slots[Slot]);
                    Slots[Slot] = nullptr;
                }
            }

            FreeSlots.clear();
            FreeSlots.reserve(Slots.size());
            for (int32 Slot = (int32)Slots.size() - 1; Slot >= 0; --Slot)
            {
                FreeSlots.push_back(Slot);
            }
            LiveCount = 0;
        }

        int32 GetLiveCount() const { return LiveCount; }
        int32 GetSlotCapacity() const { return (int32)Slots.size(); }

    private:

        bool IsValidSlot(int32 Slot) const
        {
            return Slot >= 0 && (size_t)Slot < Slots.size();
        }

        void NoteMutation()
        {
            if (!Threading::IsMainThread())
            {
                ++OffMainThreadMutations;
            }
        }

        // Temporary diagnostic: a slot the table never issued means either a stale/corrupt object or a race.
        void ReportBadSlot(const char* Site, const CObjectBase* Object, int32 Slot) const
        {
            if (bReportedBadSlot)
            {
                return;
            }
            bReportedBadSlot = true;

            LOG_ERROR("ManagedInstances::{}: object {} carries slot {}, but the table has {} slots. "
                      "Object InternalIndex {}, flags {}. Caller on main thread: {}. Off-main-thread table "
                      "mutations so far: {}.",
                Site, (const void*)Object, Slot, (int32)Slots.size(), Object->GetInternalIndex(),
                (uint32)Object->GetFlags(), Threading::IsMainThread(), OffMainThreadMutations);
        }

        int32 AcquireSlot()
        {
            if (!FreeSlots.empty())
            {
                const int32 Slot = FreeSlots.back();
                FreeSlots.pop_back();
                return Slot;
            }
            Slots.push_back(nullptr);
            Owners.push_back(nullptr);
            return (int32)Slots.size() - 1;
        }

        void FreeHandle(void* Handle)
        {
            if (Handle != nullptr && FreeHandleFn != nullptr)
            {
                FreeHandleFn(Handle);
            }
        }

        TVector<void*>                    Slots;
        TVector<CObjectBase*>             Owners;   // back-reference, so ReleaseAll can clear slot indices
        TVector<int32>                    FreeSlots;
        int32                             LiveCount = 0;
        ManagedInstances::FFreeHandleFn   FreeHandleFn = nullptr;
        uint64                            OffMainThreadMutations = 0;
        mutable bool                      bReportedBadSlot = false;
    };

    namespace ManagedInstances
    {
        void SetFreeHandleFn(FFreeHandleFn Fn)
        {
            FManagedInstanceTable::Get().SetFreeHandleFn(Fn);
        }

        void* Find(const CObjectBase* Object)
        {
            return FManagedInstanceTable::Get().Find(Object);
        }

        void Set(CObjectBase* Object, void* Handle)
        {
            FManagedInstanceTable::Get().Set(Object, Handle);
        }

        void Release(CObjectBase* Object)
        {
            FManagedInstanceTable::Get().Release(Object);
        }

        void ReleaseAll()
        {
            FManagedInstanceTable::Get().ReleaseAll();
        }

        int32 GetLiveCount()
        {
            return FManagedInstanceTable::Get().GetLiveCount();
        }

        int32 GetSlotCapacity()
        {
            return FManagedInstanceTable::Get().GetSlotCapacity();
        }
    }
}
