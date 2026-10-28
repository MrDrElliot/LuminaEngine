#pragma once

#include "imgui.h"
#include "Containers/Function.h"
#include "Containers/String.h"
#include "Containers/Vector.h"
#include "GUID/GUID.h"

namespace Lumina
{
    class FEditorTool;
    class CObject;
    class CClass;
    class CPackage;
    class FAssetRegistry;
    struct FAssetData;
}

namespace Lumina
{
    class IEditorToolContext
    {
    public:

        IEditorToolContext() = default;
        virtual ~IEditorToolContext() = default;
        
        virtual void PushModal(const FString& Title, ImVec2 Size, TMoveOnlyFunction<bool()> DrawFunction) = 0;

        virtual void OpenAssetEditor(const FGuid& AssetGUID) = 0;

        // Opens a tool for a non-CObject file (e.g. .rml) by extension; falls back to
        // the platform launcher when no editor is registered for it.
        virtual void OpenFileEditor(FStringView VirtualPath) = 0;

        virtual void OpenScriptEditor(FStringView ScriptPath) = 0;
        // The open file editor for VirtualPath, or null.
        virtual FEditorTool* FindFileEditor(FStringView VirtualPath) const = 0;

        /** Reveals a VFS path in the Content Browser: shows the panel, navigates to the containing
         *  folder, then selects and scrolls to the item. */
        virtual void BrowseToAsset(FStringView VirtualPath) = 0;

        /** The asset highlighted in the Content Browser right now, or null when nothing (or something
         *  that isn't an asset) is selected. Backs the "use the selected asset" button on object
         *  reference properties -- the counterpart to BrowseToAsset. */
        virtual const FAssetData* GetContentBrowserSelectedAsset() const = 0;

        virtual void OnDestroyAsset(CObject* InAsset) = 0;

        /** Writes every dirty package through its open tool, which is what a standalone launch reads from disk. */
        virtual void SaveAllDirtyPackages() = 0;

        // Saves the dirty ones among Packages the way SaveAllDirtyPackages saves everything.
        virtual void SavePackages(TVector<CPackage*> Packages) = 0;

        // Opens a property matrix over the assets, for viewing and editing their shared properties side by side.
        virtual void OpenPropertyMatrix(TVector<FGuid> Assets) = 0;

        // Opens the Settings panel on one settings class, such as CCodeEditorShortcuts.
        virtual void OpenSettings(CClass* Page) = 0;

        // Asks before touching files that are read-only on disk and runs Then once they are writable. True when none were, so the caller goes ahead now.
        virtual bool ConfirmWritable(TVector<FString> VirtualPaths, TMoveOnlyFunction<void()> Then, bool bThenSaves = true) = 0;
        
    };
}
