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
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"

namespace Lumina
{
    class CEdNodeGraph;

    class EDITOR_API FAssetEditorTool : public FEditorTool
    {
    public:

        FAssetEditorTool(IEditorToolContext* Context, const FString& AssetName, CObject* InAsset, CWorld* InWorld = nullptr);

        // For a tool on raw content that is not a CObject, such as a .rml file, which owns its storage and its OnSave.
        FAssetEditorTool(IEditorToolContext* Context, const FString& DisplayName);

        ~FAssetEditorTool() override;
        FAssetEditorTool(const FAssetEditorTool&) = delete;
        FAssetEditorTool& operator=(const FAssetEditorTool&) = delete;
        FAssetEditorTool(FAssetEditorTool&&) = delete;
        FAssetEditorTool& operator=(FAssetEditorTool&&) = delete;

        void OnInitialize() override;
        void Deinitialize(const FUpdateContext& UpdateContext) override;
        void Update(const FUpdateContext& UpdateContext) override;
        void VisitObjectReferences(FObjectReferenceVisitor::FSlotFunc Func) override;
        void OnSave() override;
        void OnPostUndoRedo() override;
        void OnExternalPropertyEdit(CObject* Object, FProperty* Property) override;
        FName GetToolName() const override;
        bool IsAssetEditorTool() const override;
        FFixedString GetAssetVirtualPath() const override;
        NODISCARD bool IsUnsavedDocument() override;
        bool HasLostAsset() const override { return bOpenedOnAsset && !Asset.IsValid(); }

        virtual void OnAssetLoadFinished() {}

        // The asset's data was replaced from outside, such as by a reimport; overrides drop their own caches and call the base.
        virtual void OnAssetDataChangedExternally();

        // One completed property edit, a released drag or an atomic commit, never once per frame of a drag.
        virtual void OnPropertyEditFinished(const FPropertyChangedEvent& Event) {}

        // Every edit that marks the package dirty becomes an undo step unless a tool opts out, as one holding bulk data should.
        NODISCARD virtual bool WantsAutomaticAssetUndo() { return AsSceneEditor() == nullptr; }

        // What an automatic undo step stores of the asset, its reflected properties unless a tool edits data beyond them.
        virtual void SerializeAssetForUndo(FArchive& Ar, CObject* InAsset);

        // Gives a secondary details table the same undo the main one has, snapshotting whatever object it shows.
        void WirePropertyTableUndo(FPropertyTable& Table);

        // For a tool that edits through its own widgets rather than the table, since dirtying the package alone saves a change without showing it.
        void NotifyAssetDataChanged();

        // The object the details panel is showing, or null when it shows a plain struct.
        NODISCARD CObject* GetPropertyTableObject() const;

        FPropertyTable* GetPropertyTable() { return &PropertyTable; }
        bool HasAsset() const { return Asset.IsValid(); }

    protected:

        template<Concept::IsACObject T>
        T* GetAsset()
        {
            return Cast<T>(Asset.Get());
        }

        TStrongObjectPtr<CObject>   Asset;
        mutable FFixedString        AssetVirtualPath;
        FPropertyTable              PropertyTable;
        uint8                       bAssetLoadBroadcasted : 1;
        uint8                       bOpenedOnAsset : 1;

        // Held per tool rather than polled, since the swap happens on a worker and there is no frame to notice it on.
        FDelegateHandle             AssetDataChangedHandle;

        // The "DisplayName###GUID" window title, rebuilt only when the asset is renamed.
        mutable FName               CachedWindowName;
        mutable FName               CachedWindowNameSource;

    private:

        void SetupPropertyUndo();
        void SetupAutoAssetUndo();
        FObjectSnapshotCommand::FSerializer MakeAssetUndoSerializer();
        void SubscribeToAssetDataChanges();
        void UnsubscribeFromAssetDataChanges();

        // Records any change to the asset since the last step, once no drag or widget edit is still in flight.
        void CommitUntrackedAssetEdits();

        // Taken lazily, so a step recorded elsewhere or an undo only marks it stale.
        TVector<uint8>                      AssetUndoImage;
        TVector<TStrongObjectPtr<CObject>>  AssetUndoReferenced;
        uint32                              AssetUndoGeneration = 0;
        bool                                bAssetUndoImageStale = true;
        bool                                bAutoAssetUndoOff = false;

        // The graph whose node an open property edit belongs to, which commits that edit as its own step.
        TWeakObjectPtr<CEdNodeGraph>        PropertyEditGraph;
    };
}
