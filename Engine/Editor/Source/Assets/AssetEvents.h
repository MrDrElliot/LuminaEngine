#pragma once

// EDITOR_API is defined on the build command line; see EditorPCH.h.
#include "Core/Delegates/Delegate.h"

namespace Lumina
{
    class CObject;

    DECLARE_MULTICAST_DELEGATE(FAssetDataChangedDelegate, CObject*);

    namespace AssetEvents
    {
        /** Fired when an asset's DATA is replaced underneath whatever is already looking at it -- a
         *  reimport, an external re-cook. The object identity does not change, which is exactly why this
         *  is needed: nothing about the reference goes stale, so nobody re-resolves anything, and every
         *  cache keyed off the OLD contents (property rows, unwraps, preview components) quietly survives
         *  into a frame where it no longer describes the asset.
         *
         *  NOT a "this asset was edited" signal. A tool that made the edit already knows; this is for the
         *  ones that did not. */
        EDITOR_API FAssetDataChangedDelegate& OnAssetDataChanged();

        /** Broadcasts OnAssetDataChanged. Null is ignored, so callers do not have to guard a failed load. */
        EDITOR_API void BroadcastAssetDataChanged(CObject* Asset);
    }
}
