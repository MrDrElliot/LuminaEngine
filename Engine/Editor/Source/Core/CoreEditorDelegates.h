#pragma once

// EDITOR_API is defined on the build command line; see EditorPCH.h.
#include "Containers/String.h"
#include "Core/Delegates/Delegate.h"

namespace Lumina
{
    class CObject;
    class CWorld;

    // The editor-only counterpart to FCoreDelegates; nothing here fires in a packaged game.
    struct FCoreEditorDelegates
    {
        //~ Assets, broadcast beside the FAssetRegistry call each operation already makes.

        // A tool holding edits the asset does not have yet must flush them here, or they miss the save.
        EDITOR_API static TMulticastDelegate<void, CObject*> OnAssetPreSave;

        // After the package reached disk. Not fired when the save failed.
        EDITOR_API static TMulticastDelegate<void, CObject*> OnAssetSaved;

        // A factory produced a new asset and it has been saved.
        EDITOR_API static TMulticastDelegate<void, CObject*> OnAssetCreated;

        // VFS path of what was removed. The object is already gone, so the path is all there is.
        EDITOR_API static TMulticastDelegate<void, FStringView> OnAssetDeleted;

        // Old path, then new. Covers both packages and text assets.
        EDITOR_API static TMulticastDelegate<void, FStringView, FStringView> OnAssetRenamed;

        // A project finished loading and its content is mounted. Fires again on every project switch.
        EDITOR_API static TMulticastDelegate<void> OnProjectLoaded;

        //~ Play in editor. The world is the PIE world, not the editor world it was duplicated from.

        // After the PIE world exists and the viewport has been rebound to it.
        EDITOR_API static TMulticastDelegate<void, CWorld*> OnPIEBegin;

        // Before the PIE world is torn down, while it is still valid to read.
        EDITOR_API static TMulticastDelegate<void, CWorld*> OnPIEEnd;
    };
}
