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

    // Exactly one cache line. Every object in the engine is one of these.
    static_assert(sizeof(CObjectBase) == 64, "CObjectBase left its cache line; re-check the member order.");

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

        void SetIsHandleAliveFn(ManagedInstances::FIsHandleAliveFn Fn)
        {
            IsHandleAliveFn = Fn;
        }

        void* Find(const CObjectBase* Object) const
        {
            if (Object == nullptr || Object->ManagedInstanceSlot == Constants::kIndexNone)
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

        uint32 GetHandleGeneration() const
        {
            return HandleGeneration.load(std::memory_order_relaxed);
        }

        void* FindScriptTwin(const CObjectBase* Object) const
        {
            if (Object == nullptr || Object->ManagedInstanceSlot == Constants::kIndexNone)
            {
                return nullptr;
            }

            void* Handle = nullptr;
            bool bWeak = false;
            {
                FScopeLock Lock(Mutex);

                const int32 Slot = Object->ManagedInstanceSlot;
                if (!IsValidSlot(Slot) || Slot >= (int32)ScriptTwin.size() || !ScriptTwin[Slot])
                {
                    return nullptr;
                }
                Handle = Slots[Slot];
                bWeak = WeakTwin[Slot];
            }

            // Asked outside the lock, since it calls into the managed runtime.
            if (bWeak && IsHandleAliveFn != nullptr && !IsHandleAliveFn(Handle))
            {
                return nullptr;
            }
            return Handle;
        }

        bool AdoptScriptTwin(CObjectBase* Object, void* WeakHandle)
        {
            if (Object == nullptr || WeakHandle == nullptr || Object->ManagedInstanceSlot == Constants::kIndexNone)
            {
                return false;
            }

            NoteMutation();

            void* Replaced = nullptr;
            {
                FScopeLock Lock(Mutex);

                const int32 Slot = Object->ManagedInstanceSlot;
                if (!IsValidSlot(Slot) || !ScriptTwin[Slot] || WeakTwin[Slot] || Object->GetStrongRefCount() != 0)
                {
                    return false;
                }
                HandleGeneration.fetch_add(1u, std::memory_order_relaxed);
                Replaced = Slots[Slot];
                Slots[Slot] = WeakHandle;
                WeakTwin[Slot] = true;
            }

            FreeHandle(Replaced);
            return true;
        }

        bool IsScriptTwin(const CObjectBase* Object) const
        {
            if (Object == nullptr || Object->ManagedInstanceSlot == Constants::kIndexNone)
            {
                return false;
            }

            FScopeLock Lock(Mutex);

            const int32 Slot = Object->ManagedInstanceSlot;
            return IsValidSlot(Slot) && Slot < (int32)ScriptTwin.size() && ScriptTwin[Slot];
        }

        void Set(CObjectBase* Object, void* Handle, bool bScriptTwin)
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

            // A weak wrapper must never take the slot from a script's own instance, or the script goes dark.
            if (!bScriptTwin && IsScriptTwin(Object))
            {
                FreeHandle(Handle);
                return;
            }

            NoteMutation();

            // Freeing re-enters the managed runtime, so it happens after the lock is dropped.
            void* Replaced = nullptr;
            {
                FScopeLock Lock(Mutex);

                const int32 Existing = Object->ManagedInstanceSlot;
                if (Existing != Constants::kIndexNone && !IsValidSlot(Existing))
                {
                    ReportBadSlot("Set", Object, Existing);
                    Object->ManagedInstanceSlot = Constants::kIndexNone;
                }

                if (Object->ManagedInstanceSlot != Constants::kIndexNone)
                {
                    // Replacing an instance (the previous wrapper was collected, or was the wrong type).
                    HandleGeneration.fetch_add(1u, std::memory_order_relaxed);
                    Replaced = Slots[Object->ManagedInstanceSlot];
                    Slots[Object->ManagedInstanceSlot] = Handle;
                    ScriptTwin[Object->ManagedInstanceSlot] = bScriptTwin;
                    WeakTwin[Object->ManagedInstanceSlot] = false;
                }
                else
                {
                    Object->ManagedInstanceSlot = AcquireSlot();
                    Slots[Object->ManagedInstanceSlot] = Handle;
                    ScriptTwin[Object->ManagedInstanceSlot] = bScriptTwin;
                    WeakTwin[Object->ManagedInstanceSlot] = false;
                    Owners[Object->ManagedInstanceSlot] = Object;
                    ++LiveCount;
                }
            }

            FreeHandle(Replaced);
        }

        void Release(CObjectBase* Object)
        {
            if (Object == nullptr || Object->ManagedInstanceSlot == Constants::kIndexNone)
            {
                return;
            }

            NoteMutation();

            void* Released = nullptr;
            {
                FScopeLock Lock(Mutex);

                const int32 Slot = Object->ManagedInstanceSlot;
                Object->ManagedInstanceSlot = Constants::kIndexNone;

                // A slot the table never handed out must not reach FreeSlots; it would be reissued as a valid one.
                if (!IsValidSlot(Slot))
                {
                    ReportBadSlot("Release", Object, Slot);
                    return;
                }

                HandleGeneration.fetch_add(1u, std::memory_order_relaxed);
                Released = Slots[Slot];
                Slots[Slot] = nullptr;
                ScriptTwin[Slot] = false;
                WeakTwin[Slot] = false;
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
                HandleGeneration.fetch_add(1u, std::memory_order_relaxed);

                // Cleared through the back-reference list, so a later Find sees Constants::kIndexNone, not a recycled slot.
                for (int32 Slot = 0; Slot < (int32)Slots.size(); ++Slot)
                {
                    if (Owners[Slot] != nullptr)
                    {
                        Owners[Slot]->ManagedInstanceSlot = Constants::kIndexNone;
                        Owners[Slot] = nullptr;
                    }
                    if (Slots[Slot] != nullptr)
                    {
                        Released.push_back(Slots[Slot]);
                        Slots[Slot] = nullptr;
                    }
                    ScriptTwin[Slot] = false;
                    WeakTwin[Slot] = false;
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
            ScriptTwin.push_back(false);
            WeakTwin.push_back(false);
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
        TVector<bool>                     ScriptTwin;  // a script's own instance, which a weak wrapper may not displace
        TVector<bool>                     WeakTwin;    // a twin C# adopted, alive only while C# references it
        TVector<CObjectBase*>             Owners;   // back-reference, so ReleaseAll can clear slot indices
        TVector<int32>                    FreeSlots;
        int32                             LiveCount = 0;
        ManagedInstances::FFreeHandleFn   FreeHandleFn = nullptr;
        ManagedInstances::FIsHandleAliveFn IsHandleAliveFn = nullptr;
        TAtomic<uint64>                   OffMainThreadMutations{0};
        TAtomic<uint32>                   HandleGeneration{1};
        mutable bool                      bReportedBadSlot = false;
    };

    namespace ManagedInstances
    {
        void SetFreeHandleFn(FFreeHandleFn Fn)
        {
            FManagedInstanceTable::Get().SetFreeHandleFn(Fn);
        }

        void SetIsHandleAliveFn(FIsHandleAliveFn Fn)
        {
            FManagedInstanceTable::Get().SetIsHandleAliveFn(Fn);
        }

        bool AdoptScriptTwin(CObjectBase* Object, void* WeakHandle)
        {
            return FManagedInstanceTable::Get().AdoptScriptTwin(Object, WeakHandle);
        }

        void* Find(const CObjectBase* Object)
        {
            return FManagedInstanceTable::Get().Find(Object);
        }

        bool IsScriptTwin(const CObjectBase* Object)
        {
            return FManagedInstanceTable::Get().IsScriptTwin(Object);
        }

        void* FindScriptTwin(const CObjectBase* Object)
        {
            return FManagedInstanceTable::Get().FindScriptTwin(Object);
        }

        uint32 GetHandleGeneration()
        {
            return FManagedInstanceTable::Get().GetHandleGeneration();
        }

        void Set(CObjectBase* Object, void* Handle, bool bScriptTwin)
        {
            FManagedInstanceTable::Get().Set(Object, Handle, bScriptTwin);
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
