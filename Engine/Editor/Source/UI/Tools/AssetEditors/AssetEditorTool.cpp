#include "AssetEditorTool.h"
#include "Core/Object/Package/Package.h"
#include "GUID/GUID.h"
#include "Thumbnails/ThumbnailManager.h"
#include "Tools/UI/ImGui/ImGuiX.h"
#include "UI/Tools/Transactions/ObjectSnapshotCommand.h"

#include <format>

namespace Lumina
{
    void FAssetEditorTool::OnInitialize()
    {
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
        PropertyTable.SetFinishEditCallback([this](const FPropertyChangedEvent&)
        {
            GetTransactionManager().CommitTransaction();
        });
    }

    void FAssetEditorTool::Deinitialize(const FUpdateContext& UpdateContext)
    {
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
                CachedWindowName = std::format("{0} {1}###{2}",
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

        if (CPackage::SavePackage(Asset->GetPackage(), Asset->GetPackage()->GetPackagePath()))
        {
            FAssetRegistry::Get().AssetSaved(Asset);
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
