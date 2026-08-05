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
    class EDITOR_API FAssetEditorTool : public FEditorTool
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
                   NotifyAssetDataChanged();
                });

                // Property edits on the asset become undoable CObject snapshots -- undo for free, no per-editor code.
                SetupPropertyUndo();
            }

            // Subscribed HERE and not in OnInitialize: every asset editor overrides OnInitialize to build
            // its windows and none of them call the base, so a subscription there would silently never
            // happen for any tool in the editor.
            SubscribeToAssetDataChanges();
        }

        // For tools on raw non-CObject content (e.g. .rml). Subclasses override OnSave
        // and own their backing storage; PropertyTable stays empty.
        FAssetEditorTool(IEditorToolContext* Context, const FString& DisplayName)
            : FAssetEditorTool(Context, DisplayName, nullptr, nullptr)
        {
        }


        ~FAssetEditorTool() override;

        void OnInitialize() override;
        void Deinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;

        FName GetToolName() const override;
        virtual void OnAssetLoadFinished() { }

        /** This tool's asset had its data replaced by something else (a reimport, an external re-cook).
         *  The base rebuilds the property table, which is the state that goes stale for every tool -- a
         *  reimport can change the shape of the reflected data (mesh material slots being the obvious one)
         *  and the table's rows describe the old shape until something rebuilds them.
         *
         *  Override to drop tool-specific caches too, and call the base. */
        virtual void OnAssetDataChangedExternally();
        void OnSave() override;

        bool IsAssetEditorTool() const override;
        FFixedString GetAssetVirtualPath() const override;
        FPropertyTable* GetPropertyTable() { return &PropertyTable; }

        bool HasAsset() const { return Asset.IsValid(); }

        /** Every asset editor reports its package's dirty state. Without this the base returned false for
         *  all of them, so MarkDirty was doing its job and nothing ever showed it -- no tab dot, no prompt
         *  on close. Only the world and RmlUi editors had overridden it. */
        NODISCARD bool IsUnsavedDocument() override
        {
            return Asset.IsValid() && Asset->GetPackage() != nullptr && Asset->GetPackage()->IsDirty();
        }

        /** Call after mutating this asset's data. The property table's post-edit callback routes here, but
         *  a tool that draws its own widgets (the mesh editor's LOD threshold rows, say) never goes through
         *  that table and has to call this itself -- marking the package dirty alone only makes the change
         *  SAVE, it does not make it VISIBLE. */
        void NotifyAssetDataChanged()
        {
            if (!Asset.IsValid())
            {
                return;
            }

            Asset->GetPackage()->MarkDirty();

            // Mesh and material assets are interned into FMeshResolveCache by pointer identity, so editing
            // one in place changes nothing the cache key can see and every world instance keeps drawing the
            // old resolve (stale LOD thresholds, meshlet ranges, materials). The epoch is the invalidation.
            if (Asset->IsA<CMesh>() || Asset->IsA<CMaterialInterface>())
            {
                FMeshResolveCache::BumpEpoch();
            }
        }

    private:

        // Wires the PropertyTable start/finish edit callbacks to record a CObject snapshot transaction.
        void SetupPropertyUndo();

        void SubscribeToAssetDataChanges();
        void UnsubscribeFromAssetDataChanges();

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

        // Subscription to AssetEvents::OnAssetDataChanged, dropped in Deinitialize. Held per tool rather
        // than polled, because the swap happens on a worker and there is no frame the tool could notice it.
        FDelegateHandle             AssetDataChangedHandle;

        // Cached "DisplayName###GUID" ImGui window title; rebuilt only when the asset is renamed.
        mutable FName               CachedWindowName;
        mutable FName               CachedWindowNameSource;
    };
}
