#pragma once
#include "ObjectArray.h"
#include "Core/Templates/LuminaTemplate.h"


namespace Lumina
{

    template <typename T, int = sizeof(T)>
    char (&ResolveTypeIsComplete(int))[2];

    template <typename T>
    char (&ResolveTypeIsComplete(...))[1];
    
    // Strong, owning reference to a CObject, a CObject-specific shared_ptr. While any TObjectPtr holds
    // an object it keeps it alive (intrusive strong refcount in GObjectArray), so dereferencing is safe:
    // the object cannot be freed out from under you. The last TObjectPtr to release triggers destruction.
    //
    // Layout invariant: exactly one T* (pointer-sized). FObjectProperty serialization and the Lua bindings
    // read a TObjectPtr<T> member as a raw T* at offset 0, do NOT add members.
    //
    // For a NON-owning reference that must survive the object being destroyed elsewhere, use
    // TWeakObjectPtr instead: it validates on access and returns null once the object is gone.
    template<typename T>
    class TObjectPtr
    {
    private:
        T* Object = nullptr;

        // Tag for adopting an already-incremented strong ref (no extra AddRef). Used by the weak->strong
        // upgrade, which increments atomically inside the object array before handing the pointer back.
        struct FAdoptRef {};
        TObjectPtr(T* InObject, FAdoptRef) : Object(InObject) {}

        void AddRefInternal()
        {
            if (Object)
            {
                GObjectArray.AddStrongRef((CObjectBase*)Object);
            }
        }

        void ReleaseInternal()
        {
            if (Object)
            {
                GObjectArray.ReleaseStrongRef((CObjectBase*)Object);
                Object = nullptr;
            }
        }

    public:
        TObjectPtr() = default;

        TObjectPtr(T* InObject) : Object(InObject)
        {
            AddRefInternal();
        }

        TObjectPtr(const TObjectPtr& Other) : Object(Other.Object)
        {
            AddRefInternal();
        }

        TObjectPtr(TObjectPtr&& Other) noexcept : Object(Other.Object)
        {
            Other.Object = nullptr;
        }

        template<typename U>
        requires std::is_base_of_v<T, U>
        TObjectPtr(const TObjectPtr<U>& Other) : Object(Other.Get())
        {
            AddRefInternal();
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
                Object = Other.Object;
                AddRefInternal();
            }
            return *this;
        }

        TObjectPtr& operator=(TObjectPtr&& Other) noexcept
        {
            if (this != &Other)
            {
                ReleaseInternal();
                Object = Other.Object;
                Other.Object = nullptr;
            }
            return *this;
        }

        TObjectPtr& operator=(T* InObject)
        {
            if (Object != InObject)
            {
                ReleaseInternal();
                Object = InObject;
                AddRefInternal();
            }
            return *this;
        }

        TObjectPtr& operator=(nullptr_t)
        {
            ReleaseInternal();
            return *this;
        }

        T* Get() const { return Object; }
        T* operator->() const { return Object; }
        T& operator*() const { return *Object; }
        
        explicit operator bool() const { return Object != nullptr; }
        operator T*() const { return Object; }

        bool IsValid() const { return Object != nullptr; }

        FObjectHandle GetHandle() const
        {
            return Object ? GObjectArray.GetHandleByObject(Object) : FObjectHandle();
        }

        // Release ownership without decrementing ref count
        T* Detach()
        {
            T* Temp = Object;
            Object = nullptr;
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
            return TObjectPtr<T>(static_cast<T*>(Obj), typename TObjectPtr<T>::FAdoptRef{});
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

namespace eastl
{
    template <typename T>
    struct hash<Lumina::TObjectPtr<T>>
    {
        size_t operator()(const Lumina::TObjectPtr<T>& Object) const noexcept
        {
            return eastl::hash<T*>{}(Object.Get());
        }
    };
}

namespace eastl
{
    template <typename T>
    struct hash<Lumina::TWeakObjectPtr<T>>
    {
        size_t operator()(const Lumina::TWeakObjectPtr<T>& Object) const noexcept
        {
            return eastl::hash<Lumina::FObjectHandle>{}(Object.GetHandle());
        }
    };
}