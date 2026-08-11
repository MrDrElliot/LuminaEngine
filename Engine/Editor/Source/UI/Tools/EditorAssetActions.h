#pragma once

#include "Containers/Array.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    class CClass;
    class CObject;
    class IEditorToolContext;
    struct FAssetData;
}

namespace Lumina
{
    // What an action is handed when it runs. Asset is the registry entry for the item that was
    // right-clicked; the action loads it itself if it needs the object (most do not -- a GUID and a
    // path is usually enough, and not loading keeps the menu cheap to draw).
    struct FAssetActionContext
    {
        IEditorToolContext* ToolContext = nullptr;
        const FAssetData*   Asset       = nullptr;
    };

    // One entry in an asset's right-click menu.
    struct FAssetAction
    {
        // Menu text. Include the icon in the string, as the rest of the content browser's menu does.
        FString Label;

        // Optional grey-out test. Null means always enabled.
        TFunction<bool(const FAssetActionContext&)> CanExecute;

        TFunction<void(const FAssetActionContext&)> Execute;
    };

    class EDITOR_API FAssetActionRegistry
    {
    public:

        static FAssetActionRegistry& Get();

        void RegisterAction(CClass* AssetClass, FAssetAction Action);

        // Appends every action registered for AssetClass or any of its bases, most-derived first.
        void GatherActions(CClass* AssetClass, TVector<const FAssetAction*>& Out) const;

        THashMap<CClass*, TVector<FAssetAction>> Actions;
    };

    // Registers the engine's own asset actions. Called once during editor startup, alongside the
    // built-in tool registrations.
    void RegisterBuiltinAssetActions();


    EDITOR_API FFixedString MakeSiblingAssetPath(FStringView SourceVirtualPath, const char* Suffix);
    
    EDITOR_API CObject* DuplicateAssetPackage(CObject* PrimaryAsset, FStringView DestPath);
}
