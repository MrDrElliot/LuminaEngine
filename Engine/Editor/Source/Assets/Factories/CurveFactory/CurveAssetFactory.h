#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Curve/CurveAsset.h"
#include "CurveAssetFactory.generated.h"


namespace Lumina
{
    REFLECT()
    class CCurveAssetFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Curve"; }
        FStringView GetDefaultAssetCreationName() override { return "NewCurve"; }
        FString GetAssetDescription() const override { return "Keyframed float curve evaluated by gameplay and animation systems."; }
        CClass* GetAssetClass() const override { return CCurveAsset::StaticClass(); }
        FString GetCategory() const override { return "Data"; }
    };
}
