#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Foliage/GrassType.h"
#include "GrassTypeFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CGrassTypeFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Grass Type"; }
        FStringView GetDefaultAssetCreationName() override { return "NewGrassType"; }
        FString GetAssetDescription() const override { return "A grass species a terrain material's GrassOutput node scatters onto a painted layer."; }
        CClass* GetAssetClass() const override { return CGrassType::StaticClass(); }
        FString GetCategory() const override { return "Foliage"; }
    };
}
