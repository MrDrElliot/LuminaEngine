#include "EditorPCH.h"
#include "MaterialFunctionFactory.h"
#include "Assets/AssetTypes/MaterialFunction/MaterialFunction.h"

namespace Lumina
{
    CObject* CMaterialFunctionFactory::CreateNew(const FName& Name, CPackage* Package)
    {
        return NewObject<CMaterialFunction>(Package, Name);
    }
}
