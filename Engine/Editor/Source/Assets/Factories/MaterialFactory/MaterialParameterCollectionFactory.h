#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Material/MaterialParameterCollection.h"
#include "MaterialParameterCollectionFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CMaterialParameterCollectionFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Material Parameter Collection"; }
        FStringView GetDefaultAssetCreationName() override { return "NewParameterCollection"; }
        FString GetAssetDescription() const override
        {
            return "Scalars and vectors shared by every material that binds this collection, so one write "
                   "reaches every surface without touching a material or an instance.";
        }
        CClass* GetAssetClass() const override { return CMaterialParameterCollection::StaticClass(); }
        FString GetCategory() const override { return "Material"; }
    };
}
