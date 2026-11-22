#pragma once
#include "ObjectReferenceInternal.h"
#include "Core/Templates/LuminaTemplate.h"
#include "Core/LuminaMacros.h"


namespace Lumina
{

    template <typename T, int = sizeof(T)>
    char (&ResolveTypeIsComplete(int))[2];

    template <typename T>
    char (&ResolveTypeIsComplete(...))[1];
    
    // Owning reference that refcounts through the object's array entry, which outlives the object itself.
    template<typename T>
    class TStrongObjectPtr
    {
    private:
        T*             Object = nullptr;
        Object::Internal::FCObjectEntry* Entry  = nullptr;

        // Adopts a strong ref the object array already incremented, as the weak to strong upgrade does.
        struct FAdoptRef {};
        TStrongObjectPtr(T* InObject, Object::Internal::FCObjectEntry* InEntry, FAdoptRef) : Object(InObject), Entry(InEntry) {}

        // Resolved once, so every later add and release goes through the entry instead of the object.
        void AcquireInternal(T* InObject)
        {
            Object = InObject;
            Entry  = (InObject != nullptr) ? Object::Internal::GetEntry((const CObjectBase*)InObject) : nullptr;

            // An entry-less object is pre-registration, so there is nothing to refcount and nothing to refuse.
            if (Entry != nullptr && !Entry->AddStrongRefIfAlive())
            {
                Object = nullptr;
                Entry  = nullptr;
            }
        }

        // Copying a source that outlived its object yields null, never a claim on the slot's new occupant.
        void AdoptInternal(T* InObject, Object::Internal::FCObjectEntry* InEntry)
        {
            if (InEntry != nullptr && InEntry->GetObj() != (const CObjectBase*)InObject)
            {
                Object = nullptr;
                Entry  = nullptr;
                return;
            }

            Object = InObject;
            Entry  = InEntry;
            if (Entry != nullptr && !Entry->AddStrongRefIfAlive())
            {
                Object = nullptr;
                Entry  = nullptr;
            }
        }

        // Takes the pair by value so an assignment can drop its old reference after adopting the new one.
        static void ReleaseRef(T* InObject, Object::Internal::FCObjectEntry* InEntry)
        {
            if (InEntry != nullptr)
            {
                Object::Internal::ReleaseStrongRefEntry(InEntry, (const CObjectBase*)InObject);
            }
        }

        // Cleared before releasing, since the release can destroy this pointer's owner and run this destructor again.
        void ReleaseInternal()
        {
            T* const             OldObject = Object;
            Object::Internal::FCObjectEntry* const OldEntry  = Entry;
            Object = nullptr;
            Entry  = nullptr;
            ReleaseRef(OldObject, OldEntry);
        }

    public:
        TStrongObjectPtr() = default;

        TStrongObjectPtr(T* InObject)
        {
            AcquireInternal(InObject);
        }

        TStrongObjectPtr(const TStrongObjectPtr& Other)
        {
            AdoptInternal(Other.Object, Other.Entry);
        }

        TStrongObjectPtr(TStrongObjectPtr&& Other) noexcept : Object(Other.Object), Entry(Other.Entry)
        {
            Other.Object = nullptr;
            Other.Entry  = nullptr;
        }

        template<typename U>
        requires std::is_base_of_v<T, U>
        TStrongObjectPtr(const TStrongObjectPtr<U>& Other)
        {
            AdoptInternal(Other.Object, Other.Entry);
        }

        ~TStrongObjectPtr()
        {
            ReleaseInternal();
        }

        // Releasing first would destroy the owner the source lives inside, so the new reference is taken first.
        TStrongObjectPtr& operator=(const TStrongObjectPtr& Other)
        {
            // Reassigning what is already held is common in per-frame code and would otherwise cost two atomics.
            if (this != &Other && (Object != Other.Object || Entry != Other.Entry))
            {
                T* const             OldObject = Object;
                Object::Internal::FCObjectEntry* const OldEntry  = Entry;
                AdoptInternal(Other.Object, Other.Entry);
                ReleaseRef(OldObject, OldEntry);
            }
            return *this;
        }

        TStrongObjectPtr& operator=(TStrongObjectPtr&& Other) noexcept
        {
            if (this != &Other)
            {
                T* const             OldObject = Object;
                Object::Internal::FCObjectEntry* const OldEntry  = Entry;
                Object = Other.Object;
                Entry  = Other.Entry;
                Other.Object = nullptr;
                Other.Entry  = nullptr;
                ReleaseRef(OldObject, OldEntry);
            }
            return *this;
        }

        TStrongObjectPtr& operator=(T* InObject)
        {
            if (Object != InObject)
            {
                T* const             OldObject = Object;
                Object::Internal::FCObjectEntry* const OldEntry  = Entry;
                AcquireInternal(InObject);
                ReleaseRef(OldObject, OldEntry);
            }
            return *this;
        }

        TStrongObjectPtr& operator=(std::nullptr_t)
        {
            ReleaseInternal();
            return *this;
        }

        // Null once the slot has moved on, which means this reference outlived what it pointed at.
        T* Get() const noexcept
        {
            if (Entry == nullptr)
            {
                return Object;
            }
            return (Entry->GetObj() == (const CObjectBase*)Object) ? Object : nullptr;
        }

        // True while the target has no array entry, so nothing refcounts it and Get cannot check it.
        bool IsUnregistered() const noexcept { return Object != nullptr && Entry == nullptr; }

        T* operator->() const noexcept { return Get(); }
        T& operator*() const noexcept { return *Get(); }

        explicit operator bool() const noexcept { return Get() != nullptr; }

        bool IsValid() const noexcept { return Get() != nullptr; }

        FObjectHandle GetHandle() const
        {
            T* Live = Get();
            return Live ? Object::Internal::GetHandleByObject(Live) : FObjectHandle();
        }

        void Reset()
        {
            ReleaseInternal();
        }

        // All routed through Get, so equality agrees with what a dereference gives, stale slot or not.
        bool operator==(const TStrongObjectPtr& Other) const noexcept { return Get() == Other.Get(); }
        bool operator==(T* Other) const noexcept { return Get() == Other; }
        bool operator==(std::nullptr_t) const noexcept { return Get() == nullptr; }

        template<typename U> friend class TStrongObjectPtr;
        template<typename U> friend class TWeakObjectPtr;
    };


    
    template<typename T>
    class TWeakObjectPtr
    {
    private:
        // Entries never die and the generation is what invalidates a handle, so a weak ref needs no count.
        FObjectHandle Handle;

        void ClearHandle()
        {
            Handle = FObjectHandle();
        }

    public:
        TWeakObjectPtr() = default;

        TWeakObjectPtr(T* InObject)
        {
            if (InObject)
            {
                Handle = Object::Internal::GetHandleByObject(InObject);
            }
        }

        TWeakObjectPtr(const FObjectHandle& InHandle) : Handle(InHandle)
        {
        }

        TWeakObjectPtr(const TStrongObjectPtr<T>& Strong) : Handle(Strong.GetHandle())
        {
        }

        TWeakObjectPtr(const TWeakObjectPtr& Other) = default;

        TWeakObjectPtr(TWeakObjectPtr&& Other) noexcept : Handle(Other.Handle)
        {
            Other.ClearHandle();
        }

        template<typename U>
        requires std::is_base_of_v<T, U>
        TWeakObjectPtr(const TWeakObjectPtr<U>& Other) : Handle(Other.Handle)
        {
        }

        ~TWeakObjectPtr() = default;

        TWeakObjectPtr& operator=(const TWeakObjectPtr& Other) = default;

        TWeakObjectPtr& operator=(TWeakObjectPtr&& Other) noexcept
        {
            if (this != &Other)
            {
                Handle = Other.Handle;
                Other.ClearHandle();
            }
            return *this;
        }

        TWeakObjectPtr& operator=(T* InObject)
        {
            Handle = (InObject != nullptr) ? Object::Internal::GetHandleByObject(InObject) : FObjectHandle();
            return *this;
        }

        TWeakObjectPtr& operator=(const TStrongObjectPtr<T>& Strong)
        {
            Handle = Strong.GetHandle();
            return *this;
        }

        TWeakObjectPtr& operator=(std::nullptr_t)
        {
            ClearHandle();
            return *this;
        }

        // Atomic validate and acquire inside the object array, so unlike Get() then wrap it never races a destroy.
        TStrongObjectPtr<T> Lock() const
        {
            CObjectBase* Obj = Object::Internal::TryAddStrongRef(Handle);
            if (Obj == nullptr)
            {
                return TStrongObjectPtr<T>();
            }
            // TryAddStrongRef already incremented the strong count; adopt it without a second AddRef.
            return TStrongObjectPtr<T>(static_cast<T*>(Obj), Object::Internal::GetEntry(Obj),
                typename TStrongObjectPtr<T>::FAdoptRef{});
        }

        T* Get() const
        {
            return (T*)Object::Internal::ResolveHandle(Handle);
        }

        bool IsValid() const
        {
            return Handle.IsValid() && Get() != nullptr;
        }

        bool IsStale() const
        {
            return Handle.IsValid() && Get() == nullptr;
        }

        FObjectHandle GetHandle() const { return Handle; }

        void Reset()
        {
            ClearHandle();
        }

        bool operator==(const TWeakObjectPtr& Other) const { return Handle == Other.Handle; }
        bool operator!=(const TWeakObjectPtr& Other) const { return Handle != Other.Handle; }
        bool operator==(std::nullptr_t) const { return !IsValid(); }
        bool operator!=(std::nullptr_t) const { return IsValid(); }

        template<typename U> friend class TWeakObjectPtr;
    };
}

namespace Lumina
{
    template <typename T>
    NODISCARD FORCEINLINE uint64 GetTypeHash(const TStrongObjectPtr<T>& Object) noexcept
    {
        return GetTypeHash(Object.Get());
    }

    template <typename T>
    NODISCARD FORCEINLINE uint64 GetTypeHash(const TWeakObjectPtr<T>& Object) noexcept
    {
        return GetTypeHash(Object.GetHandle());
    }
}
