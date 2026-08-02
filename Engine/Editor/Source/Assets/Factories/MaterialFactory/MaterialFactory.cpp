#include "EditorPCH.h"
#include "MaterialFactory.h"
#include "Assets/AssetTypes/Material/Material.h"
#include "Core/Serialization/Package/PackageSaver.h"
#include "Paths/Paths.h"

namespace Lumina
{
    CObject* CMaterialFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CMaterial>(Package, Name);
    }
}
