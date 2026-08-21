#include "EditorPCH.h"
#include "AssetEvents.h"

namespace Lumina::AssetEvents
{
    FAssetDataChangedDelegate& OnAssetDataChanged()
    {
        // Function-local, since a static-init ordering hazard against editor tool construction would be silent.
        static FAssetDataChangedDelegate Delegate;
        return Delegate;
    }

    void BroadcastAssetDataChanged(CObject* Asset)
    {
        if (Asset == nullptr)
        {
            return;
        }

        OnAssetDataChanged().Broadcast(Asset);
    }
}
