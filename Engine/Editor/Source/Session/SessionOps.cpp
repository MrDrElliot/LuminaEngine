#include "EditorPCH.h"

#include "Session/SessionOps.h"

#include "Core/Engine/Engine.h"
#include "Core/Object/ObjectCore.h"
#include "Core/Object/ObjectIterator.h"
#include "Core/Object/Package/Package.h"
#include "World/World.h"
#include "LuminaEditor.h"
#include "UI/EditorUI.h"
#include "UI/Tools/EditorTool.h"
#include "UI/Tools/WorldEditorTool.h"

namespace Lumina::SessionOps
{
    const char* const GNoSceneEditor = "No world editor is open.";

    namespace
    {
        FEditorUI* FindUI()
        {
            return GEditorEngine != nullptr
                ? static_cast<FEditorUI*>(GEditorEngine->GetDevelopmentToolsUI())
                : nullptr;
        }

        FWorldEditorTool* FindSceneEditor()
        {
            FEditorUI* UI = FindUI();
            return UI != nullptr ? UI->FindTool<FWorldEditorTool>() : nullptr;
        }

        FWorldEditorTool* RequireSceneEditor(FString& OutError)
        {
            FWorldEditorTool* Tool = FindSceneEditor();
            if (Tool == nullptr)
            {
                OutError = GNoSceneEditor;
            }
            return Tool;
        }
    }

    IEditorToolContext* GetToolContext()
    {
        return FindUI();
    }

    bool HasSceneEditor()
    {
        return FindSceneEditor() != nullptr;
    }

    ECS::FRegistry* GetSceneRegistry(FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        return Tool != nullptr ? &Tool->GetSceneEntityRegistry() : nullptr;
    }

    CWorld* GetSceneWorld(FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        return Tool != nullptr ? Tool->GetSceneWorld() : nullptr;
    }

    bool IsSimulating()
    {
        FWorldEditorTool* Tool = FindSceneEditor();
        return Tool != nullptr && Tool->HasSimulatingWorld();
    }

    bool GetPlayState(FPlayState& Out, FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }

        Out.bPlaying = Tool->HasSimulatingWorld();
        Out.bPaused  = Out.bPlaying && Tool->IsPlaySessionPaused();

        if (CWorld* World = Tool->GetSceneWorld(); World != nullptr && World->GetPackage() != nullptr)
        {
            Out.World = FString(World->GetPackage()->GetPackagePath().c_str());
        }
        return true;
    }

    bool StartPlay(FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }
        if (!Tool->StartPlayInEditor())
        {
            OutError = "The play session could not be started.";
            return false;
        }
        return true;
    }

    bool SetPlayNetwork(int32 NumPlayers, FStringView NetMode, FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }

        ENetMode Mode;
        if (NetMode == "standalone")     { Mode = ENetMode::Standalone; }
        else if (NetMode == "listen")    { Mode = ENetMode::ListenServer; }
        else if (NetMode == "dedicated") { Mode = ENetMode::DedicatedServer; }
        else
        {
            OutError = "NetMode must be standalone, listen or dedicated.";
            return false;
        }

        Tool->SetPlayNetwork(NumPlayers, Mode);
        return true;
    }

    bool StopPlay(FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }
        Tool->StopAllSimulations();
        return true;
    }

    bool SetPaused(bool bPaused, FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }
        Tool->SetPlaySessionPaused(bPaused);
        return true;
    }

    bool IsPaused(FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        return Tool != nullptr && Tool->IsPlaySessionPaused();
    }

    bool RunTransacted(FName Label, const TFunction<void()>& Mutate, FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }
        Tool->RunTransacted(Label, Mutate);
        return true;
    }

    bool RunCreationTransacted(FName Label, const TFunction<void()>& Mutate, FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }
        Tool->RunCreationTransacted(Label, Mutate);
        return true;
    }

    bool RunDestroyTransacted(FName Label, const TVector<ECS::FEntity>& Doomed,
        const TFunction<void()>& Mutate, FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }
        Tool->RunDestroyTransacted(Label, Doomed, Mutate);
        return true;
    }

    bool RemoveComponentTransacted(FName Label, ECS::FEntity Entity, CStruct* ComponentType, FString& OutError)
    {
        FWorldEditorTool* Tool = RequireSceneEditor(OutError);
        if (Tool == nullptr)
        {
            return false;
        }
        Tool->RemoveComponentTransacted(Label, Entity, ComponentType);
        return true;
    }

    bool RunObjectTransacted(CObject* Object, FName Label, const TFunction<void()>& Mutate)
    {
        FEditorUI* UI = FindUI();
        FEditorTool* Owner = UI != nullptr ? UI->FindAssetEditor(Object) : nullptr;
        if (Owner == nullptr)
        {
            return false;
        }
        Owner->RunObjectTransacted(Label, Object, Mutate);
        return true;
    }

    FUndoState GetUndoState()
    {
        FUndoState State;
        if (FWorldEditorTool* Tool = FindSceneEditor())
        {
            const FName Undo = Tool->PeekUndoLabel();
            const FName Redo = Tool->PeekRedoLabel();
            if (!Undo.IsNone()) { State.NextUndo = Undo.ToString(); }
            if (!Redo.IsNone()) { State.NextRedo = Redo.ToString(); }
        }
        return State;
    }

    int32 Undo(int32 Steps)
    {
        FWorldEditorTool* Tool = FindSceneEditor();
        int32 Applied = 0;
        for (int32 Index = 0; Tool != nullptr && Index < Steps && Tool->RunUndo(); ++Index)
        {
            ++Applied;
        }
        return Applied;
    }

    int32 Redo(int32 Steps)
    {
        FWorldEditorTool* Tool = FindSceneEditor();
        int32 Applied = 0;
        for (int32 Index = 0; Tool != nullptr && Index < Steps && Tool->RunRedo(); ++Index)
        {
            ++Applied;
        }
        return Applied;
    }

    void ForEachTab(const TFunction<void(const FTabInfo&)>& Functor)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr || !Functor)
        {
            return;
        }

        UI->ForEachTab([&Functor](const FEditorUI::FTabInfo& Tab)
        {
            FTabInfo Out;
            Out.Name      = Tab.Name;
            Out.Id        = Tab.Id;
            Out.AssetGuid = Tab.AssetGuid;
            Out.bUnsaved  = Tab.bUnsaved;
            Out.bFocused  = Tab.bFocused;
            Out.bClosable = Tab.bClosable;
            Functor(Out);
        });
    }

    bool FocusTab(FStringView Name, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr)
        {
            OutError = GNoSceneEditor;
            return false;
        }
        return UI->FocusTab(Name, OutError);
    }

    CWorld* GetTabWorld(FStringView Name, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr)
        {
            OutError = GNoSceneEditor;
            return nullptr;
        }

        FEditorTool* Tool = UI->FindTab(Name, OutError);
        if (Tool == nullptr)
        {
            return nullptr;
        }

        if (Tool->GetWorld() == nullptr)
        {
            OutError = Lumina::Format("'{}' has no world to capture.", Name);
        }
        return Tool->GetWorld();
    }

    bool FloatTab(FStringView Name, const FVector2& ScreenPosition, const FVector2& Size, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr)
        {
            OutError = GNoSceneEditor;
            return false;
        }
        return UI->FloatTab(Name, ScreenPosition, Size, OutError);
    }

    bool BrowseContentFolder(FStringView Folder, FStringView Search, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr || !UI->BrowseToFolder(Folder, Search))
        {
            OutError = "The content browser is not open.";
            return false;
        }
        return true;
    }

    bool CloseTab(FStringView Name, bool bDiscardUnsaved, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr)
        {
            OutError = GNoSceneEditor;
            return false;
        }
        return UI->CloseTab(Name, bDiscardUnsaved, OutError);
    }

    TVector<FString> GetDirtyPackagePaths()
    {
        TVector<FString> Paths;
        for (TObjectIterator<CPackage> Itr; Itr; ++Itr)
        {
            CPackage* Package = *Itr;
            if (!Package->HasAnyFlag(OF_MarkedDestroy) && Package->IsDirty())
            {
                Paths.push_back(FString(Package->GetPackagePath().c_str()));
            }
        }
        return Paths;
    }

    bool SaveAll(uint32& OutSaved, TVector<FString>& OutFailedPaths, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr)
        {
            OutError = "The editor UI is not running.";
            return false;
        }

        FEditorUI::FSaveAllResult Result = UI->SaveAllDirtyPackagesSilently();
        OutSaved = Result.Saved;
        OutFailedPaths = Move(Result.FailedPaths);
        if (!OutFailedPaths.empty())
        {
            OutError = Lumina::Format("{} package(s) failed to save.", OutFailedPaths.size());
            return false;
        }
        return true;
    }

    bool Quit(EQuitUnsaved Unsaved, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr)
        {
            OutError = "The editor UI is not running.";
            return false;
        }

        if (Unsaved == EQuitUnsaved::Save)
        {
            uint32 Saved = 0;
            TVector<FString> Failed;
            if (!SaveAll(Saved, Failed, OutError))
            {
                return false;
            }
        }
        else if (Unsaved == EQuitUnsaved::Refuse)
        {
            const TVector<FString> Dirty = GetDirtyPackagePaths();
            if (!Dirty.empty())
            {
                OutError = Lumina::Format("{} package(s) have unsaved changes.", Dirty.size());
                return false;
            }
        }

        // Shutting down under a live play session tears its world down out of order.
        if (IsSimulating())
        {
            FString StopError;
            (void)StopPlay(StopError);
        }

        UI->QuitWithoutPrompt();
        return true;
    }

    bool OpenAsset(const FGuid& AssetGUID, FString& OutTabId, FString& OutError)
    {
        FEditorUI* UI = FindUI();
        if (UI == nullptr)
        {
            OutError = "The editor UI is not running.";
            return false;
        }

        UI->OpenAssetEditor(AssetGUID);

        CObject* Asset = FindObject<CObject>(AssetGUID);
        FEditorTool* Tool = Asset != nullptr ? UI->FindAssetEditor(Asset) : nullptr;
        if (Tool == nullptr && Asset != nullptr && Asset->IsA<CWorld>())
        {
            Tool = UI->FindTool<FWorldEditorTool>();
        }

        if (Tool == nullptr)
        {
            OutError = "No editor opened for that asset.";
            return false;
        }

        // The window name carries its id after a "###", which is what a caller addresses a tab by.
        const FString Full(Tool->GetToolName().c_str());
        const size_t Hash = Full.find("###");
        OutTabId = Hash == FString::npos ? Full : Full.substr(Hash + 3);
        return true;
    }
}
