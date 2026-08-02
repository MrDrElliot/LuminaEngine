#pragma once

#include "Containers/Name.h"
#include "Containers/String.h"
#include "ModuleAPI.h"
#include "Platform/GenericPlatform.h"
#include "entt/entt.hpp"

namespace Lumina
{
    class CObject;
    class CClass;
    class CWorld;
    struct FAssetData;
}

// Single typed drag-drop channel ("LumDD") for editor and runtime UIs; replaces ad-hoc
// per-tool payload strings. Set* on the source side, Accept*/PeekPayload on the target.
namespace Lumina::DragDrop
{
    enum class EPayloadKind : uint8
    {
        None = 0,
        Asset,
        Entity,
        File,
    };

    struct RUNTIME_API FPayload
    {
        EPayloadKind    Kind = EPayloadKind::None;

        FName           AssetClassName;
        FFixedString    AssetPath;
        CObject*        AssetObject = nullptr;

        CWorld*         World = nullptr;
        entt::entity    Entity = entt::null;

        FFixedString    FilePath;
        FFixedString    FileExtension;
    };

    constexpr const char* GImGuiPayloadType = "LumDD";

    // Source helpers
    RUNTIME_API void SetAssetPayload(FName ClassName, FStringView Path, CObject* Object = nullptr);
    RUNTIME_API void SetAssetPayload(const FAssetData& Asset);
    RUNTIME_API void SetAssetPayload(CObject* Asset);
    RUNTIME_API void SetEntityPayload(CWorld* World, entt::entity Entity);
    RUNTIME_API void SetFilePayload(FStringView VirtualPath);

    // Target helpers

    // Read-only inspection of the payload being dragged; null when no LumDD drag is active.
    RUNTIME_API const FPayload* PeekPayload();

    // True only on the frame the mouse releases over an active drop target.
    RUNTIME_API bool IsDelivered();

    // Class-checked asset accept; returns the resolved CObject only on delivery when the
    // dragged asset is-a Class. Lazily loads if the source gave no loaded object.
    RUNTIME_API CObject* AcceptAssetOfClass(CClass* Class);

    template <typename T>
    T* AcceptAsset()
    {
        return static_cast<T*>(AcceptAssetOfClass(T::StaticClass()));
    }

    // Entity drop. On delivery, writes outputs and returns true.
    RUNTIME_API bool AcceptEntity(CWorld** OutWorld, entt::entity* OutEntity);

    // Plain file drop. ExtensionFilter is empty for "any file", or e.g. "luau",
    // "rml" (lowercase, no dot) to gate by extension.
    RUNTIME_API bool AcceptFile(FStringView ExtensionFilter, FFixedString& OutPath);

    // Convenience for script slots: accepts .lua or .luau files.
    RUNTIME_API bool AcceptScript(FFixedString& OutPath);

    // Internal: cleared when no drag is active so a stale payload does not
    // leak across frames. Safe to call from a per-frame top-level place.
    RUNTIME_API void EndFrameTick();
}
