#pragma once

#include "Containers/Function.h"
#include "Containers/Name.h"
#include "Core/Object/ObjectHandleTyped.h"


namespace Lumina
{
    class CObject;
    class FArchive;

    /** Gameplay-facing identity for an EAssetFlags::Primary asset, keyed by FAssetName (e.g. "Hero").
     *  One Name maps to at most one Primary asset; duplicates are a registry error. */
    struct RUNTIME_API FPrimaryAssetId
    {
        FPrimaryAssetId() = default;
        explicit FPrimaryAssetId(const FName& InName) : Name(InName) {}

        bool        IsValid() const { return !Name.IsNone(); }
        const FName& GetName() const { return Name; }

        bool operator==(const FPrimaryAssetId& Other) const { return Name == Other.Name; }
        bool operator!=(const FPrimaryAssetId& Other) const { return Name != Other.Name; }

        friend RUNTIME_API FArchive& operator<<(FArchive& Ar, FPrimaryAssetId& Self);

    protected:

        FName Name;
    };


    /** Typed wrapper; narrows resolve/load results back to T*. */
    template<typename T>
    struct TPrimaryAssetId : FPrimaryAssetId
    {
        TPrimaryAssetId() = default;
        explicit TPrimaryAssetId(const FName& InName) : FPrimaryAssetId(InName) {}
        explicit TPrimaryAssetId(const FPrimaryAssetId& InBase) : FPrimaryAssetId(InBase) {}

        TObjectPtr<T> LoadSynchronous() const;
        void          LoadAsync(const TFunction<void(T*)>& Callback) const;
    };
}

// LoadSynchronous/LoadAsync impls live in AssetManager.h (need full FAssetManager; avoids circular include).

namespace eastl
{
    template<>
    struct hash<Lumina::FPrimaryAssetId>
    {
        size_t operator()(const Lumina::FPrimaryAssetId& P) const noexcept
        {
            return eastl::hash<Lumina::FName>{}(P.GetName());
        }
    };
}
