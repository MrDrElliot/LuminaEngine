#include "Containers/StringFormat.h"
#include "AssetEditorTool.h"
#include "Core/CoreEditorDelegates.h"
#include "Assets/AssetEvents.h"
#include "Core/Object/Package/Package.h"
#include "GUID/GUID.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"
#include "UI/Tools/NodeGraph/EdGraphNode.h"
#include "UI/Tools/NodeGraph/EdNodeGraph.h"
#include "Core/Object/Cast.h"


namespace Lumina
{
    void FAssetEditorTool::OnInitialize()
    {
    }

    void FAssetEditorTool::SubscribeToAssetDataChanges()
    {
        // Every open tool hears every change, which is the point, but the base only acts on its own.
        AssetDataChangedHandle = AssetEvents::OnAssetDataChanged().AddLambda([this](CObject* Changed)
        {
            if (Changed != nullptr && Changed == Asset.Get())
            {
                OnAssetDataChangedExternally();
            }
        });
    }

    void FAssetEditorTool::UnsubscribeFromAssetDataChanges()
    {
        if (AssetDataChangedHandle.IsValid())
        {
            AssetEvents::OnAssetDataChanged().Remove(AssetDataChangedHandle);
            AssetDataChangedHandle = {};
        }
    }

    void FAssetEditorTool::OnAssetDataChangedExternally()
    {
        if (!Asset.IsValid())
        {
            return;
        }

        // A reimport can change the reflected shape, so SetObject rebuilds rows and handles both.
        PropertyTable.SetObject(Asset.Get(), Asset->GetClass());
        PropertyTable.MarkDirty();
    }

    void FAssetEditorTool::SetupPropertyUndo()
    {
        WirePropertyTableUndo(PropertyTable);
    }

    void FAssetEditorTool::WirePropertyTableUndo(FPropertyTable& Table)
    {
        // The table often shows a graph node or another sub-object, so the snapshot follows what it shows.
        Table.SetStartEditCallback([this, &Table](const FPropertyChangedEvent& Event)
        {
            const CStruct* Type = Table.GetType();
            CObject* Edited = Type != nullptr && Type->IsA<CClass>() ? static_cast<CObject*>(Table.GetObject()) : Asset.Get();
            if (Edited == nullptr)
            {
                return;
            }

            // A node's graph records it, so the canvas and the panel never make two steps of one change.
            CEdGraphNode* Node = Cast<CEdGraphNode>(Edited);
            CEdNodeGraph* Graph = Node != nullptr ? Node->GetOwningGraph() : nullptr;
            if (Graph != nullptr && Graph->GetTransactionManager() == &GetTransactionManager())
            {
                PropertyEditGraph = Graph;
                Graph->BeginLongEdit();
                return;
            }

            FTransactionManager& Manager = GetTransactionManager();
            Manager.BeginTransaction(Event.PropertyName);
            Manager.Record(MakeUnique<FObjectSnapshotCommand>(Edited, Event.PropertyName));
        });

        // On edit end, commit, and the command self-drops if nothing actually changed.
        Table.SetFinishEditCallback([this](const FPropertyChangedEvent& Event)
        {
            if (CEdNodeGraph* Graph = PropertyEditGraph.Get())
            {
                PropertyEditGraph = nullptr;
                Graph->NotifyContentChanged();
                Graph->EndLongEdit(Event.PropertyName);
            }
            else
            {
                GetTransactionManager().CommitTransaction();
            }

            // After the commit, so a tool's response is its own transaction rather than joining the edit's.
            OnPropertyEditFinished(Event);
        });
    }

    void FAssetEditorTool::SerializeAssetForUndo(FArchive& Ar, CObject* InAsset)
    {
        InAsset->GetClass()->SerializeTaggedProperties(Ar, InAsset);
    }

    FObjectSnapshotCommand::FSerializer FAssetEditorTool::MakeAssetUndoSerializer()
    {
        return [this](FArchive& Ar, CObject* InAsset) { SerializeAssetForUndo(Ar, InAsset); };
    }

    void FAssetEditorTool::SetupAutoAssetUndo()
    {
        FTransactionManager& Manager = GetTransactionManager();
        Manager.OnCommitted = [this]() { bAssetUndoImageStale = true; };
        Manager.OnPostApply = [this]()
        {
            bAssetUndoImageStale = true;
            OnPostUndoRedo();
        };
    }

    void FAssetEditorTool::CommitUntrackedAssetEdits()
    {
        if (bAutoAssetUndoOff || !Asset.IsValid() || Asset->GetPackage() == nullptr)
        {
            return;
        }

        if (!WantsAutomaticAssetUndo())
        {
            bAutoAssetUndoOff = true;
            return;
        }

        FTransactionManager& Manager = GetTransactionManager();
        const uint32 Generation = Asset->GetPackage()->GetEditGeneration();

        if (bAssetUndoImageStale)
        {
            if (Manager.IsRecording())
            {
                return;
            }
            FObjectSnapshotCommand::Capture(Asset.Get(), AssetUndoImage, MakeAssetUndoSerializer());
            AssetUndoReferenced.clear();
            FObjectSnapshotCommand::CollectReferenced(Asset.Get(), AssetUndoReferenced);
            AssetUndoGeneration = Generation;
            bAssetUndoImageStale = false;
            return;
        }

        if (Generation == AssetUndoGeneration)
        {
            return;
        }

        // A drag or a field being typed into is one step, taken when it ends.
        if (Manager.IsRecording() || ImGui::IsAnyMouseDown() || ImGui::IsAnyItemActive())
        {
            return;
        }

        TVector<uint8> Current;
        FObjectSnapshotCommand::Capture(Asset.Get(), Current, MakeAssetUndoSerializer());
        AssetUndoGeneration = Generation;
        if (Current == AssetUndoImage)
        {
            return;
        }

        // Two images per step of an asset this size would make the history cost more than the asset.
        constexpr SIZE_T MaxAutomaticUndoBytes = 16ull * 1024 * 1024;
        if (Current.size() > MaxAutomaticUndoBytes)
        {
            LOG_WARN("'{}' is too large to snapshot for undo, so edits made outside its details panel are not undoable.", Asset->GetName().c_str());
            bAutoAssetUndoOff = true;
            AssetUndoImage.clear();
            return;
        }

        const FName Label(Format("Edit {}", Asset->GetName().c_str()).c_str());
        Manager.BeginTransaction(Label);
        Manager.Record(MakeUnique<FObjectSnapshotCommand>(Asset.Get(), Label, Move(AssetUndoImage), Current, MakeAssetUndoSerializer(), Move(AssetUndoReferenced)));
        Manager.CommitTransaction();

        AssetUndoImage = Move(Current);
        AssetUndoReferenced.clear();
        FObjectSnapshotCommand::CollectReferenced(Asset.Get(), AssetUndoReferenced);
        bAssetUndoImageStale = false;
    }

    CObject* FAssetEditorTool::GetPropertyTableObject() const
    {
        const CStruct* Type = PropertyTable.GetType();
        return Type != nullptr && Type->IsA<CClass>() ? static_cast<CObject*>(PropertyTable.GetObject()) : nullptr;
    }

    void FAssetEditorTool::OnPostUndoRedo()
    {
        // A restore can resize containers the rows were built from, so the table rebuilds rather than trusts them.
        PropertyTable.MarkDirty();
    }

    FAssetEditorTool::~FAssetEditorTool()
    {
        // The handle holds this, so leaving it subscribed means the next broadcast hits freed memory.
        UnsubscribeFromAssetDataChanges();
    }

    void FAssetEditorTool::Deinitialize(const FUpdateContext& UpdateContext)
    {
        UnsubscribeFromAssetDataChanges();

        FEditorTool::Deinitialize(UpdateContext);
    }

    FName FAssetEditorTool::GetToolName() const
    {
        if (Asset != nullptr)
        {
            // ImGui shows the label but hashes the stable GUID, so same-named assets do not merge.
            const FName Name = Asset->GetName();
            if (CachedWindowNameSource != Name)
            {
                CachedWindowNameSource = Name;
                CachedWindowName = Format("{0} {1}###{2}",
                    GetTitlebarIcon(), Name.c_str(), Asset->GetGUID().ToShortString().c_str()).c_str();
            }
            return CachedWindowName;
        }
        return ToolName;
    }

    void FAssetEditorTool::Update(const FUpdateContext& UpdateContext)
    {
        FEditorTool::Update(UpdateContext);

        CommitUntrackedAssetEdits();

        DrawWorldGrid();

        if (!bAssetLoadBroadcasted && Asset != nullptr)
        {
            OnAssetLoadFinished();
            bAssetLoadBroadcasted = true;
        }

        // FEditorUI dispatches Ctrl+S to the focused tool, so handling it here would fire for all of them.
    }

    void FAssetEditorTool::OnSave()
    {
        // A tool editing a raw file such as .rml overrides this to write through the VFS instead.
        if (Asset == nullptr)
        {
            return;
        }

        if (ShouldGenerateThumbnailOnSave() && Asset->GetPackage())
        {
            if (!CThumbnailManager::Get().GenerateThumbnail(Asset.Get(), Asset->GetPackage()))
            {
                GenerateThumbnail(Asset->GetPackage());
            }
        }

        FCoreEditorDelegates::OnAssetPreSave.Broadcast(Asset.Get());

        if (CPackage::SavePackage(Asset->GetPackage(), Asset->GetPackage()->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetSaved(Asset.Get());
            FCoreEditorDelegates::OnAssetSaved.Broadcast(Asset.Get());
            ImGuiX::Notifications::NotifySuccess("Successfully saved package: \"{0}\"", Asset->GetName().c_str());
        }
        else
        {
            ImGuiX::Notifications::NotifyError("Failed to save package: \"{0}\"", Asset->GetName().c_str());
        }
    }

    bool FAssetEditorTool::IsAssetEditorTool() const
    {
        return true;
    }

    FFixedString FAssetEditorTool::GetAssetVirtualPath() const
    {
        if (!Asset.IsValid())
        {
            return {};
        }

        CPackage* Package = Asset->GetPackage();
        if (Package == nullptr)
        {
            return {};
        }

        // The mount-relative virtual path with the .lasset extension, as the browser tiles carry it.
        return Package->GetPackagePath();
    }
}
