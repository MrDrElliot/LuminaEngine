#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/DataAsset/DataAsset.h"
#include "DataAssetFactory.generated.h"

namespace Lumina
{
    // One factory for every data asset type: the creation dialogue picks which CDataAsset subclass to
    // mint, so a new type is just a C++ class -- nothing to register here.
    REFLECT()
    class CDataAssetFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Data Asset"; }
        FStringView GetDefaultAssetCreationName() override { return "NewDataAsset"; }
        FString GetAssetDescription() const override { return "Designer-authored data: pick a class deriving from CDataAsset and fill in its properties."; }
        CClass* GetAssetClass() const override { return CDataAsset::StaticClass(); }
        FString GetCategory() const override { return "Data"; }

        // Pick the class the new data asset will be.
        bool HasCreationDialogue() const override;
        bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) override;

    private:

        CClass* SelectedClass = nullptr;
    };
}
