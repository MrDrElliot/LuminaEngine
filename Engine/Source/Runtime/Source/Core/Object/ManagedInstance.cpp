#include "RuntimePCH.h"
#include "ManagedInstance.h"

#include "Containers/Vector.h"
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
            return Slots[Object->ManagedInstanceSlot];
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

            const int32 Slot = Object->ManagedInstanceSlot;
            Object->ManagedInstanceSlot = INDEX_NONE;

            FreeHandle(Slots[Slot]);
            Slots[Slot] = nullptr;
            Owners[Slot] = nullptr;
            FreeSlots.push_back(Slot);
            --LiveCount;
        }

        void ReleaseAll()
        {
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
