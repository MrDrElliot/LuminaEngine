#pragma once

#include "imgui.h"
#include "Containers/Function.h"
#include "GUID/GUID.h"

namespace Lumina
{
    class CObject;
    class CClass;
    class FAssetRegistry;
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

        /** Reveals a VFS path in the Content Browser: shows the panel, navigates to the containing
         *  folder, then selects and scrolls to the item. */
        virtual void BrowseToAsset(FStringView VirtualPath) = 0;

        /** Called just before an asset is marked for destroy, mostly to close any asset editors that may be using it */
        virtual void OnDestroyAsset(CObject* InAsset) = 0;
        
    };
}
