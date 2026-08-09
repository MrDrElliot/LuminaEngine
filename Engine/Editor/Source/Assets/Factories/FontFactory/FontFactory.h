#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Font/Font.h"
#include "FontFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CFontFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;


        FString GetAssetName() const override { return "Font"; }
        FStringView GetDefaultAssetCreationName() override { return "NewFont"; }
        FString GetAssetDescription() const override { return "A TrueType / OpenType font face."; }
        FString GetCategory() const override { return "UI"; }
        CClass* GetAssetClass() const override { return CFont::StaticClass(); }
    };
}
