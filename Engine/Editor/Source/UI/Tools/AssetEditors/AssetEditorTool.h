#pragma once

#include "Core/Object/Cast.h"
#include "Core/Object/Class.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectHandleTyped.h"
#include "Core/Object/Package/Package.h"
#include "Assets/AssetTypes/Material/MaterialInterface.h"
#include "Assets/AssetTypes/Mesh/Mesh.h"
#include "UI/Properties/PropertyTable.h"
#include "UI/Tools/EditorTool.h"
#include "World/Scene/RenderScene/MeshResolveCache.h"

namespace Lumina
{
    class FAssetEditorTool : public FEditorTool
    {
    public:

        FAssetEditorTool(IEditorToolContext* Context, const FString& AssetName, CObject* InAsset, CWorld* InWorld = nullptr)
            : FEditorTool(Context, AssetName, InWorld)
            , bAssetLoadBroadcasted(false)
        {
            Asset = InAsset;
            if (InAsset != nullptr)
            {
                // SetObject auto-resolves the class CDO as the diff/reset
                // default, no explicit plumbing needed here.
                PropertyTable.SetObject(InAsset, InAsset->GetClass());
                PropertyTable.MarkDirty();
                PropertyTable.SetPostEditCallback([&](const FPropertyChangedEvent&)
                {
                   Asset->GetPackage()->MarkDirty();

                   // Mesh and material assets are interned into FMeshResolveCache by pointer identity, so
                   // editing one in place (assigning a mesh's default material slots, retargeting a
                   // material's textures) changes nothing the cache key can see. Every world instance
                   // would keep drawing the old resolve. Property edits bypass the setters that bump the
                   // epoch, so this is the chokepoint that covers all of them -- one place, every asset
                   // editor, rather than a hook per tool.
                   if (Asset->IsA<CMesh>() || Asset->IsA<CMaterialInterface>())
                   {
                       FMeshResolveCache::BumpEpoch();
                   }
                });

                // Property edits on the asset become undoable CObject snapshots -- undo for free, no per-editor code.
                SetupPropertyUndo();
            }
        }

        // For tools on raw non-CObject content (e.g. .rml). Subclasses override OnSave
        // and own their backing storage; PropertyTable stays empty.
        FAssetEditorTool(IEditorToolContext* Context, const FString& DisplayName)
            : FAssetEditorTool(Context, DisplayName, nullptr, nullptr)
        {
        }


        void OnInitialize() override;
        void Deinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;

        FName GetToolName() const override;
        virtual void OnAssetLoadFinished() { }
        void OnSave() override;

        bool IsAssetEditorTool() const override;
        FFixedString GetAssetVirtualPath() const override;
        FPropertyTable* GetPropertyTable() { return &PropertyTable; }

        bool HasAsset() const { return Asset.IsValid(); }

    private:

        // Wires the PropertyTable start/finish edit callbacks to record a CObject snapshot transaction.
        void SetupPropertyUndo();

    protected:

        template<Concept::IsACObject T>
        T* GetAsset()
        {
            return Cast<T>(Asset.Get());
        }

    protected:

        TObjectPtr<CObject>         Asset;
        FPropertyTable              PropertyTable;
        uint8                       bAssetLoadBroadcasted:1;

        // Cached "DisplayName###GUID" ImGui window title; rebuilt only when the asset is renamed.
        mutable FName               CachedWindowName;
        mutable FName               CachedWindowNameSource;
    };
}
