#include "EditorPCH.h"
#include "DataAssetSchemaFactory.h"
#include "Assets/AssetTypes/DataAsset/DataAssetSchema.h"

namespace Lumina
{
    CObject* CDataAssetSchemaFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CDataAssetSchema>(Package, Name);
    }
}
