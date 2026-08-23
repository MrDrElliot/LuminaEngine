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
        // Comparing against a probe keeps the check meaningful when a field is added, if mirrored here too.
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

            FScopeLock Lock(Mutex);

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

            // Freeing re-enters the managed runtime, so it happens after the lock is dropped.
            void* Replaced = nullptr;
            {
                FScopeLock Lock(Mutex);

                const int32 Existing = Object->ManagedInstanceSlot;
                if (Existing != INDEX_NONE && !IsValidSlot(Existing))
                {
                    ReportBadSlot("Set", Object, Existing);
                    Object->ManagedInstanceSlot = INDEX_NONE;
                }

                if (Object->ManagedInstanceSlot != INDEX_NONE)
                {
                    // Replacing an instance (the previous wrapper was collected, or was the wrong type).
                    Replaced = Slots[Object->ManagedInstanceSlot];
                    Slots[Object->ManagedInstanceSlot] = Handle;
                }
                else
                {
                    Object->ManagedInstanceSlot = AcquireSlot();
                    Slots[Object->ManagedInstanceSlot] = Handle;
                    Owners[Object->ManagedInstanceSlot] = Object;
                    ++LiveCount;
                }
            }

            FreeHandle(Replaced);
        }

        void Release(CObjectBase* Object)
        {
            if (Object == nullptr || Object->ManagedInstanceSlot == INDEX_NONE)
            {
                return;
            }

            NoteMutation();

            void* Released = nullptr;
            {
                FScopeLock Lock(Mutex);

                const int32 Slot = Object->ManagedInstanceSlot;
                Object->ManagedInstanceSlot = INDEX_NONE;

                // A slot the table never handed out must not reach FreeSlots; it would be reissued as a valid one.
                if (!IsValidSlot(Slot))
                {
                    ReportBadSlot("Release", Object, Slot);
                    return;
                }

                Released = Slots[Slot];
                Slots[Slot] = nullptr;
                Owners[Slot] = nullptr;
                FreeSlots.push_back(Slot);
                --LiveCount;
            }

            FreeHandle(Released);
        }

        void ReleaseAll()
        {
            NoteMutation();

            TVector<void*> Released;
            {
                FScopeLock Lock(Mutex);

                Released.reserve(Slots.size());

                // Cleared through the back-reference list, so a later Find sees INDEX_NONE, not a recycled slot.
                for (int32 Slot = 0; Slot < (int32)Slots.size(); ++Slot)
                {
                    if (Owners[Slot] != nullptr)
                    {
                        Owners[Slot]->ManagedInstanceSlot = INDEX_NONE;
                        Owners[Slot] = nullptr;
                    }
                    if (Slots[Slot] != nullptr)
                    {
                        Released.push_back(Slots[Slot]);
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

            for (void* Handle : Released)
            {
                FreeHandle(Handle);
            }
        }

        int32 GetLiveCount() const { FScopeLock Lock(Mutex); return LiveCount; }
        int32 GetSlotCapacity() const { FScopeLock Lock(Mutex); return (int32)Slots.size(); }

    private:

        bool IsValidSlot(int32 Slot) const
        {
            return Slot >= 0 && (size_t)Slot < Slots.size();
        }

        void NoteMutation()
        {
            if (!Threading::IsMainThread())
            {
                OffMainThreadMutations.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // A temporary diagnostic, since a slot the table never issued means a stale object or a race.
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
                (uint32)Object->GetFlags(), Threading::IsMainThread(),
                OffMainThreadMutations.load(std::memory_order_relaxed));
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

        // The render thread reaches this through the managed RenderScene bridge, so it is not game-thread only.
        mutable FMutex                    Mutex;
        TVector<void*>                    Slots;
        TVector<CObjectBase*>             Owners;   // back-reference, so ReleaseAll can clear slot indices
        TVector<int32>                    FreeSlots;
        int32                             LiveCount = 0;
        ManagedInstances::FFreeHandleFn   FreeHandleFn = nullptr;
        TAtomic<uint64>                   OffMainThreadMutations{0};
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
