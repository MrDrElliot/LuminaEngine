#pragma once
#include "Assets/Factories/Factory.h"
#include "PrefabFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CPrefabFactory : public CFactory
    {
        GENERATED_BODY()
    public:
        
        CClass* GetAssetClass() const override;
        FString GetCategory() const override { return "Gameplay"; }
        FString GetAssetName() const override { return "Prefab"; }
        FStringView GetDefaultAssetCreationName() override { return "NewPrefab"; }
        
        CObject* CreateNew(const FName& Name, CPackage* Package) override;
    
    };
}
