#pragma once
#include "ObjectArray.h"
#include "Core/Templates/LuminaTemplate.h"


namespace Lumina
{

    template <typename T, int = sizeof(T)>
    char (&ResolveTypeIsComplete(int))[2];

    template <typename T>
    char (&ResolveTypeIsComplete(...))[1];
    
    // Owning reference that refcounts through the object's array entry, which outlives the object itself.
    template<typename T>
    class TObjectPtr
    {
    private:
        T*             Object = nullptr;
        FCObjectEntry* Entry  = nullptr;

        // Adopts a strong ref the object array already incremented, as the weak to strong upgrade does.
        struct FAdoptRef {};
        TObjectPtr(T* InObject, FCObjectEntry* InEntry, FAdoptRef) : Object(InObject), Entry(InEntry) {}

        // Resolved once, so every later add and release goes through the entry instead of the object.
        void AcquireInternal(T* InObject)
        {
            Object = InObject;
            Entry  = (InObject != nullptr) ? GObjectArray.GetEntry((const CObjectBase*)InObject) : nullptr;
            if (Entry != nullptr)
            {
                Entry->AddStrongRef();
            }
        }

        // Copying a source that outlived its object yields null, never a claim on the slot's new occupant.
        void AdoptInternal(T* InObject, FCObjectEntry* InEntry)
        {
            if (InEntry != nullptr && InEntry->GetObj() != (const CObjectBase*)InObject)
            {
                Object = nullptr;
                Entry  = nullptr;
                return;
            }

            Object = InObject;
            Entry  = InEntry;
            if (Entry != nullptr)
            {
                Entry->AddStrongRef();
            }
        }

        void ReleaseInternal()
        {
            if (Entry != nullptr)
            {
                GObjectArray.ReleaseStrongRefEntry(Entry, (const CObjectBase*)Object);
                Entry = nullptr;
            }
            Object = nullptr;
        }

    public:
        TObjectPtr() = default;

        TObjectPtr(T* InObject)
        {
            AcquireInternal(InObject);
        }

        TObjectPtr(const TObjectPtr& Other)
        {
            AdoptInternal(Other.Object, Other.Entry);
        }

        TObjectPtr(TObjectPtr&& Other) noexcept : Object(Other.Object), Entry(Other.Entry)
        {
            Other.Object = nullptr;
            Other.Entry  = nullptr;
        }

        template<typename U>
        requires std::is_base_of_v<T, U>
        TObjectPtr(const TObjectPtr<U>& Other)
        {
            AdoptInternal(Other.Object, Other.Entry);
        }

        ~TObjectPtr()
        {
            ReleaseInternal();
        }

        TObjectPtr& operator=(const TObjectPtr& Other)
        {
            if (this != &Other)
            {
                ReleaseInternal();
                AdoptInternal(Other.Object, Other.Entry);
            }
            return *this;
        }

        TObjectPtr& operator=(TObjectPtr&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseInternal();
                Object = Other.Object;
                Entry  = Other.Entry;
                Other.Object = nullptr;
                Other.Entry  = nullptr;
            }
            return *this;
        }

        TObjectPtr& operator=(T* InObject)
        {
            if (Object != InObject)
            {
                ReleaseInternal();
                AcquireInternal(InObject);
            }
            return *this;
        }

        TObjectPtr& operator=(nullptr_t)
        {
            ReleaseInternal();
            return *this;
        }

        // Null once the slot has moved on, which means this reference outlived what it pointed at.
        T* Get() const
        {
            if (Entry == nullptr)
            {
                return Object;
            }
            return (Entry->GetObj() == (const CObjectBase*)Object) ? Object : nullptr;
        }

        T* operator->() const { return Get(); }
        T& operator*() const { return *Get(); }

        explicit operator bool() const { return Get() != nullptr; }
        operator T*() const { return Get(); }

        bool IsValid() const { return Get() != nullptr; }

        FObjectHandle GetHandle() const
        {
            T* Live = Get();
            return Live ? GObjectArray.GetHandleByObject(Live) : FObjectHandle();
        }

        // Release ownership without decrementing ref count
        T* Detach()
        {
            T* Temp = Object;
            Object = nullptr;
            Entry  = nullptr;
            return Temp;
        }

        void Reset()
        {
            ReleaseInternal();
        }

        bool operator==(const TObjectPtr& Other) const { return Object == Other.Object; }
        bool operator!=(const TObjectPtr& Other) const { return Object != Other.Object; }
        bool operator==(T* Other) const { return Object == Other; }
        bool operator!=(T* Other) const { return Object != Other; }
        bool operator==(nullptr_t) const { return Object == nullptr; }
        bool operator!=(nullptr_t) const { return Object != nullptr; }

        template<typename U> friend class TObjectPtr;
        template<typename U> friend class TWeakObjectPtr;
    };


    
    template<typename T>
    class TWeakObjectPtr
    {
    private:
        FObjectHandle Handle;

        void AddWeakRefInternal()
        {
            if (Handle.IsValid())
            {
                GObjectArray.AddWeakRefByIndex(Handle.Index);
            }
        }

        void ReleaseWeakRefInternal()
        {
            if (Handle.IsValid())
            {
                GObjectArray.ReleaseWeakRefByIndex(Handle.Index);
                Handle = FObjectHandle();
            }
        }

    public:
        TWeakObjectPtr() = default;

        TWeakObjectPtr(T* InObject)
        {
            if (InObject)
            {
                Handle = GObjectArray.GetHandleByObject(InObject);
                AddWeakRefInternal();
            }
        }

        TWeakObjectPtr(const FObjectHandle& InHandle) : Handle(InHandle)
        {
            AddWeakRefInternal();
        }

        TWeakObjectPtr(const TObjectPtr<T>& Strong)
        {
            Handle = Strong.GetHandle();
            AddWeakRefInternal();
        }

        TWeakObjectPtr(const TWeakObjectPtr& Other) : Handle(Other.Handle)
        {
            AddWeakRefInternal();
        }

        TWeakObjectPtr(TWeakObjectPtr&& Other) noexcept : Handle(Other.Handle)
        {
            Other.Handle = FObjectHandle();
        }

        template<typename U>
        requires std::is_base_of_v<T, U>
        TWeakObjectPtr(const TWeakObjectPtr<U>& Other) : Handle(Other.Handle)
        {
            AddWeakRefInternal();
        }

        ~TWeakObjectPtr()
        {
            ReleaseWeakRefInternal();
        }

        TWeakObjectPtr& operator=(const TWeakObjectPtr& Other)
        {
            if (this != &Other)
            {
                ReleaseWeakRefInternal();
                Handle = Other.Handle;
                AddWeakRefInternal();
            }
            return *this;
        }

        TWeakObjectPtr& operator=(TWeakObjectPtr&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseWeakRefInternal();
                Handle = Other.Handle;
                Other.Handle = FObjectHandle();
            }
            return *this;
        }

        TWeakObjectPtr& operator=(T* InObject)
        {
            ReleaseWeakRefInternal();
            if (InObject)
            {
                Handle = GObjectArray.GetHandleByObject(InObject);
                AddWeakRefInternal();
            }
            return *this;
        }

        TWeakObjectPtr& operator=(const TObjectPtr<T>& Strong)
        {
            ReleaseWeakRefInternal();
            Handle = Strong.GetHandle();
            AddWeakRefInternal();
            return *this;
        }

        TWeakObjectPtr& operator=(nullptr_t)
        {
            ReleaseWeakRefInternal();
            return *this;
        }

        // Try to get a strong reference; returns null if the object was deleted. Atomic: the validate +
        // acquire happen together inside the object array, so this never races a concurrent destroy into
        // a use-after-free (unlike Get()-then-wrap). This is the safe way to pin a weakly-held object.
        TObjectPtr<T> Lock() const
        {
            CObjectBase* Obj = GObjectArray.TryAddStrongRef(Handle);
            if (Obj == nullptr)
            {
                return TObjectPtr<T>();
            }
            // TryAddStrongRef already incremented the strong count; adopt it without a second AddRef.
            return TObjectPtr<T>(static_cast<T*>(Obj), GObjectArray.GetEntry(Obj),
                typename TObjectPtr<T>::FAdoptRef{});
        }

        T* Get() const
        {
            return (T*)GObjectArray.ResolveHandle(Handle);
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
            ReleaseWeakRefInternal();
        }

        bool operator==(const TWeakObjectPtr& Other) const { return Handle == Other.Handle; }
        bool operator!=(const TWeakObjectPtr& Other) const { return Handle != Other.Handle; }
        bool operator==(nullptr_t) const { return !IsValid(); }
        bool operator!=(nullptr_t) const { return IsValid(); }

        template<typename U> friend class TWeakObjectPtr;
    };
}

namespace Lumina
{
    template <typename T>
    NODISCARD FORCEINLINE uint64 GetTypeHash(const TObjectPtr<T>& Object) noexcept
    {
        return GetTypeHash(Object.Get());
    }

    template <typename T>
    NODISCARD FORCEINLINE uint64 GetTypeHash(const TWeakObjectPtr<T>& Object) noexcept
    {
        return GetTypeHash(Object.GetHandle());
    }
}
