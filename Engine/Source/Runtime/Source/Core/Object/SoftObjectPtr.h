#pragma once

#include "Containers/Function.h"
#include "Containers/String.h"
#include "GUID/GUID.h"
#include "ObjectHandleTyped.h"


namespace Lumina
{
    class CObject;
    class FArchive;

    /** Serializable asset reference by path; resolves on demand, never force-loads. Identity is
     *  always path-based (not GUID) so TryResolve can't move a hash bucket; CachedGUID write is atomic. */
    struct RUNTIME_API FSoftObjectPath
    {
        FSoftObjectPath() = default;
        explicit FSoftObjectPath(FStringView InPath) : Path(InPath.data(), InPath.size()) {}
        explicit FSoftObjectPath(const FGuid& InGUID) : CachedGUID(InGUID) {}
        FSoftObjectPath(FStringView InPath, const FGuid& InGUID)
            : Path(InPath.data(), InPath.size())
            , CachedGUID(InGUID)
        {}

        FSoftObjectPath(const FSoftObjectPath& Other)
            : Path(Other.Path), CachedGUID(Other.CachedGUID) {}
        FSoftObjectPath(FSoftObjectPath&& Other) noexcept
            : Path(Move(Other.Path)), CachedGUID(Other.CachedGUID) {}

        FSoftObjectPath& operator=(const FSoftObjectPath& Other)
        {
            if (this != &Other) { Path = Other.Path; CachedGUID = Other.CachedGUID; }
            return *this;
        }
        FSoftObjectPath& operator=(FSoftObjectPath&& Other) noexcept
        {
            if (this != &Other) { Path = Move(Other.Path); CachedGUID = Other.CachedGUID; }
            return *this;
        }

        bool        IsNull() const  { return Path.empty() && !CachedGUID.IsValid(); }
        bool        IsValid() const { return !IsNull(); }

        FStringView GetPath() const { return FStringView(Path.c_str(), Path.size()); }
        const FGuid& GetCachedGUID() const { return CachedGUID; }

        void SetPath(FStringView InPath)
        {
            Path.assign(InPath.data(), InPath.size());
            CachedGUID = FGuid();
        }

        void Reset()
        {
            Path.clear();
            CachedGUID = FGuid();
        }

        /** Look up Path in FAssetRegistry to populate CachedGUID. No-op if
         *  GUID already known. Returns true on success. Thread-safe. */
        bool TryResolve() const;

        /** Blocking load through FAssetManager; returns nullptr on miss. */
        CObject* LoadSynchronous() const;

        /** Fire-and-forget async load. Callback fires on completion (or
         *  immediately with nullptr if the path can't be resolved). */
        void LoadAsync(const TFunction<void(CObject*)>& Callback) const;

        bool operator==(const FSoftObjectPath& Other) const
        {
            // Path-only, stable across resolve state. See class comment.
            return Path == Other.Path;
        }
        bool operator!=(const FSoftObjectPath& Other) const { return !(*this == Other); }

        friend RUNTIME_API FArchive& operator<<(FArchive& Ar, FSoftObjectPath& Self);

    private:
        FString         Path;
        mutable FGuid   CachedGUID;
    };


    /** Typed soft pointer wrapping FSoftObjectPath; layout-identical to it
     *  (single member, no vtable), the reflector relies on this. */
    template<typename T>
    struct TSoftObjectPtr
    {
        TSoftObjectPtr() = default;
        explicit TSoftObjectPtr(FStringView InPath) : Inner(InPath) {}
        explicit TSoftObjectPtr(const FSoftObjectPath& InPath) : Inner(InPath) {}
        explicit TSoftObjectPtr(const FGuid& InGUID) : Inner(InGUID) {}

        bool        IsNull() const  { return Inner.IsNull(); }
        bool        IsValid() const { return Inner.IsValid(); }

        FStringView           GetPath() const        { return Inner.GetPath(); }
        const FGuid&          GetCachedGUID() const  { return Inner.GetCachedGUID(); }
        const FSoftObjectPath& GetSoftPath() const   { return Inner; }
        FSoftObjectPath&      GetSoftPath()          { return Inner; }

        void SetPath(FStringView InPath) { Inner.SetPath(InPath); }
        void Reset()                     { Inner.Reset(); }

        TObjectPtr<T> LoadSynchronous() const
        {
            CObject* Obj = Inner.LoadSynchronous();
            // Runtime type validation against ObjectClass happens in FSoftObjectProperty.
            static_assert(eastl::is_base_of_v<CObject, T>, "TSoftObjectPtr<T>: T must derive from CObject");
            return TObjectPtr<T>(static_cast<T*>(Obj));
        }

        void LoadAsync(const TFunction<void(T*)>& Callback) const
        {
            static_assert(eastl::is_base_of_v<CObject, T>, "TSoftObjectPtr<T>: T must derive from CObject");
            Inner.LoadAsync([Callback](CObject* Obj)
            {
                if (Callback)
                {
                    Callback(static_cast<T*>(Obj));
                }
            });
        }

        bool operator==(const TSoftObjectPtr& Other) const { return Inner == Other.Inner; }
        bool operator!=(const TSoftObjectPtr& Other) const { return Inner != Other.Inner; }

        friend FArchive& operator<<(FArchive& Ar, TSoftObjectPtr& Self) { return Ar << Self.Inner; }

    private:
        FSoftObjectPath Inner;
    };

    // Layout invariant the reflector + FSoftObjectProperty rely on: the
    // storage of TSoftObjectPtr<CObject> is one FSoftObjectPath.
    static_assert(sizeof(TSoftObjectPtr<CObject>) == sizeof(FSoftObjectPath),
        "TSoftObjectPtr<T> must be layout-identical to FSoftObjectPath");
}

namespace eastl
{
    template<>
    struct hash<Lumina::FSoftObjectPath>
    {
        size_t operator()(const Lumina::FSoftObjectPath& P) const noexcept
        {
            // Hash by path only; GUID-hashing would rebucket once TryResolve ran.
            const Lumina::FStringView V = P.GetPath();
            return eastl::hash<eastl::string_view>{}(eastl::string_view(V.data(), V.size()));
        }
    };

    template<typename T>
    struct hash<Lumina::TSoftObjectPtr<T>>
    {
        size_t operator()(const Lumina::TSoftObjectPtr<T>& P) const noexcept
        {
            return eastl::hash<Lumina::FSoftObjectPath>{}(P.GetSoftPath());
        }
    };
}
