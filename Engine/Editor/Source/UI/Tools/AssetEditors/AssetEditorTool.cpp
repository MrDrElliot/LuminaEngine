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
        // The table often shows a graph node or another sub-object, so the snapshot follows what it shows.
        PropertyTable.SetStartEditCallback([this](const FPropertyChangedEvent& Event)
        {
            CObject* Edited = GetPropertyTableObject();
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
        PropertyTable.SetFinishEditCallback([this](const FPropertyChangedEvent& Event)
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
