#include "EditorPCH.h"
#include "CurveAssetFactory.h"

namespace Lumina
{
    CObject* CCurveAssetFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CCurveAsset>(Package, Name);
    }
}
