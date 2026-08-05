#include "EditorPCH.h"
#include "AssetEvents.h"

namespace Lumina::AssetEvents
{
    FAssetDataChangedDelegate& OnAssetDataChanged()
    {
        // Function-local rather than a namespace-scope global: subscribers are editor tools, which are
        // constructed during editor startup, and a static-init ordering hazard here would be silent.
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
