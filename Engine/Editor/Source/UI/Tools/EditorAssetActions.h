#pragma once

#include "Assets/AssetRegistry/AssetData.h"
#include "Containers/HashTable.h"
#include "Containers/Name.h"
#include "Containers/Span.h"
#include "Containers/Vector.h"
#include "Containers/Function.h"
#include "Containers/String.h"

namespace Lumina
{
    class CClass;
    class CObject;
    class CPackage;
    class IEditorToolContext;
}

namespace Lumina
{
    // Asset is the right-clicked registry entry, and actions load the object only when needed to keep the menu cheap to draw.
    struct FAssetActionContext
    {
        IEditorToolContext* ToolContext = nullptr;
        FAssetDataPtr       Asset;
    };

    // One entry in an asset's right-click menu.
    struct FAssetAction
    {
        // Menu text. Include the icon in the string, as the rest of the content browser's menu does.
        FString Label;

        // Optional gray-out test. Null means always enabled.
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

    // Called once during editor startup alongside the built-in tool registrations.
    void RegisterBuiltinAssetActions();


    EDITOR_API FFixedString MakeSiblingAssetPath(FStringView SourceVirtualPath, const char* Suffix);
    
    EDITOR_API CObject* DuplicateAssetPackage(CObject* PrimaryAsset, FStringView DestPath);

    // Saves a copy of the asset at SourcePath as a free Name_Copy beside it and registers it, null when that fails.
    EDITOR_API CObject* DuplicateAsset(FStringView SourcePath, FFixedString& OutPath);

    // A whole copy that keeps DuplicateTransient ids such as graph node ids, which only need be unique within the set, repointing references between Sources to the copies.
    EDITOR_API THashMap<CObject*, CObject*> CopyObjectsInto(TSpan<CObject* const> Sources, CPackage* Dest, const TFunction<FName(CObject*)>& NameFor);
}
