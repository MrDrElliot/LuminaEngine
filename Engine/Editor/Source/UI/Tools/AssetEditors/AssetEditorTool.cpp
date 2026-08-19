#include "Containers/StringFormat.h"
#include "AssetEditorTool.h"
#include "Core/CoreEditorDelegates.h"
#include "Assets/AssetEvents.h"
#include "Core/Object/Package/Package.h"
#include "GUID/GUID.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"


namespace Lumina
{
    void FAssetEditorTool::OnInitialize()
    {
    }

    void FAssetEditorTool::SubscribeToAssetDataChanges()
    {
        // Filtered to THIS tool's asset. Every open tool hears every change, which is what makes the
        // broadcast worth having (a reimported texture reaches the material editors using it too, once
        // those subscribe), but the base only acts on its own.
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

        // Re-point rather than just MarkDirty: a reimport can change the shape of the reflected data, and
        // the table's rows (and the handles inside them) describe the old shape. SetObject rebuilds both.
        PropertyTable.SetObject(Asset.Get(), Asset->GetClass());
        PropertyTable.MarkDirty();
    }

    void FAssetEditorTool::SetupPropertyUndo()
    {
        // Start (edit begin / drag start): open a transaction and snapshot the asset's before-image.
        PropertyTable.SetStartEditCallback([this](const FPropertyChangedEvent& Event)
        {
            if (Asset != nullptr)
            {
                FTransactionManager& Manager = GetTransactionManager();
                Manager.BeginTransaction(Event.PropertyName);
                Manager.Record(MakeUnique<FObjectSnapshotCommand>(Asset.Get(), Event.PropertyName));
            }
        });

        // Finish (edit end): commit; the command captures its after-image and self-drops if nothing changed.
        PropertyTable.SetFinishEditCallback([this](const FPropertyChangedEvent& Event)
        {
            GetTransactionManager().CommitTransaction();

            // After the commit, so anything the tool does in response is a separate transaction rather than
            // landing inside the one that recorded the edit.
            OnPropertyEditFinished(Event);
        });
    }

    FAssetEditorTool::~FAssetEditorTool()
    {
        // Backstop for a tool torn down without Deinitialize: the handle holds `this`, so leaving it
        // subscribed means the next broadcast calls into freed memory.
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
            // "<Icon> Name###GUID": ImGui shows the label but hashes the stable GUID, so same-named
            // assets don't merge. The icon is part of the label rather than added by the base class,
            // because this override replaces the name the base composed -- without it here, every
            // asset tab would be the only unlabeled kind of tab in the editor.
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

        // Ctrl+S is dispatched centrally by FEditorUI to the focused tool only;
        // doing it here would fire OnSave on every open tool simultaneously.
    }

    void FAssetEditorTool::OnSave()
    {
        // Default save path applies to CObject-backed assets only. Tools that
        // edit raw files (e.g. .rml) override this to write through the VFS.
        if (Asset == nullptr)
        {
            return;
        }

        if (ShouldGenerateThumbnailOnSave() && Asset->GetPackage())
        {
            if (!CThumbnailManager::Get().GenerateThumbnail(Asset, Asset->GetPackage()))
            {
                GenerateThumbnail(Asset->GetPackage());
            }
        }

        FCoreEditorDelegates::OnAssetPreSave.Broadcast(Asset);

        if (CPackage::SavePackage(Asset->GetPackage(), Asset->GetPackage()->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetSaved(Asset);
            FCoreEditorDelegates::OnAssetSaved.Broadcast(Asset);
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

        // Package path is the mount-relative virtual path with the .lasset extension, the same form
        // the content browser's tile items carry.
        return Package->GetPackagePath();
    }
}
