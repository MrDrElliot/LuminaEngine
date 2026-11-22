#pragma once

#include "World/ECS/Registry.h"


#include "Containers/Name.h"
#include "Containers/String.h"
#include "ModuleAPI.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    class CObject;
    class CClass;
    class CStruct;
    class CWorld;
    struct FAssetData;
}

// Single typed drag-drop channel ("LumDD") with Set* on the source side and Accept*/PeekPayload on the target.
namespace Lumina::DragDrop
{
    enum class EPayloadKind : uint8
    {
        None = 0,
        Asset,
        Entity,
        SceneFolder,
        File,
        // An entity the drop creates, named EntityName, carrying ComponentType or seeded from AssetObject when set.
        NewEntity,
        // An entry of a tool's palette, such as a collision shape type, named by its palette and index.
        PaletteItem,
    };

    struct RUNTIME_API FPayload
    {
        EPayloadKind    Kind = EPayloadKind::None;

        FName           AssetClassName;
        FFixedString    AssetPath;
        CObject*        AssetObject = nullptr;

        CWorld*         World = nullptr;
        ECS::FEntity    Entity = ECS::NullEntity;
        uint32          SceneFolderID = 0;

        FFixedString    FilePath;
        FFixedString    FileExtension;

        FName           EntityName;
        CStruct*        ComponentType = nullptr;

        FName           PaletteName;
        int32           PaletteIndex = -1;

        // Set once the UI that started the drag has closed, so something else draws the drag preview.
        bool            bSourceClosed = false;
    };

    constexpr const char* GImGuiPayloadType = "LumDD";

    // Source helpers
    RUNTIME_API void SetAssetPayload(FName ClassName, FStringView Path, CObject* Object = nullptr);
    RUNTIME_API void SetAssetPayload(const FAssetData& Asset);
    RUNTIME_API void SetAssetPayload(CObject* Asset);
    RUNTIME_API void SetEntityPayload(CWorld* World, ECS::FEntity Entity);
    RUNTIME_API void SetSceneFolderPayload(CWorld* World, uint32 FolderID);
    RUNTIME_API void SetFilePayload(FStringView VirtualPath);
    RUNTIME_API void SetNewEntityPayload(FName EntityName, CStruct* ComponentType = nullptr, CObject* Seed = nullptr);
    RUNTIME_API void SetPaletteItemPayload(FName Palette, int32 Index);

    // For a source that closes mid-drag, such as a popup the cursor left, so the drag carries on without it.
    RUNTIME_API void MarkSourceClosed();

    // Target helpers

    // Read-only inspection of the payload being dragged; null when no LumDD drag is active.
    RUNTIME_API const FPayload* PeekPayload();

    // True only on the frame the mouse releases over an active drop target.
    RUNTIME_API bool IsDelivered();

    // Returns the resolved CObject only on delivery of an asset that is-a Class, loading it lazily if needed.
    RUNTIME_API CObject* AcceptAssetOfClass(CClass* Class);

    template <typename T>
    T* AcceptAsset()
    {
        return static_cast<T*>(AcceptAssetOfClass(T::StaticClass()));
    }

    // Entity drop. On delivery, writes outputs and returns true.
    RUNTIME_API bool AcceptEntity(CWorld** OutWorld, ECS::FEntity* OutEntity);

    // Outliner folder drop. On delivery, writes outputs and returns true.
    RUNTIME_API bool AcceptSceneFolder(CWorld** OutWorld, uint32* OutFolderID);

    // Plain file drop. ExtensionFilter is empty for any file, or a lowercase extension without the dot such as "rml".
    RUNTIME_API bool AcceptFile(FStringView ExtensionFilter, FFixedString& OutPath);

    // A palette entry from Palette, refused before it highlights anything when the drag carries something else.
    RUNTIME_API bool AcceptPaletteItem(FName Palette, int32& OutIndex);

    // Internal. Clears the payload when no drag is active so it cannot leak across frames.
    RUNTIME_API void EndFrameTick();
}
