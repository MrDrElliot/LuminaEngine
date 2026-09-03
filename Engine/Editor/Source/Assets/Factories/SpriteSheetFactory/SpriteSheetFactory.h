#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/SpriteSheet/SpriteSheet.h"
#include "SpriteSheetFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CSpriteSheetFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Sprite Sheet"; }
        FStringView GetDefaultAssetCreationName() override { return "NewSpriteSheet"; }
        FString GetAssetDescription() const override { return "A texture sliced into a frame grid, with named animations played over those frames."; }
        CClass* GetAssetClass() const override { return CSpriteSheet::StaticClass(); }
        FString GetCategory() const override { return "Rendering"; }
    };
}
