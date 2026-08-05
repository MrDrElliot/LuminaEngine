#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Animation/BlendSpace/BlendSpace.h"
#include "GUID/GUID.h"
#include "BlendSpaceFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CBlendSpaceFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Blend Space"; }
        FStringView GetDefaultAssetCreationName() override { return "NewBlendSpace"; }
        FString GetAssetDescription() const override { return "Animation clips placed in a 1D or 2D value space and blended by an input position."; }
        CClass* GetAssetClass() const override { return CBlendSpace::StaticClass(); }
        FString GetCategory() const override { return "Animation"; }

        // Samples are authored per skeleton, so picking one up front keeps the clip picker filtered.
        bool HasCreationDialogue() const override;
        bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) override;

    private:

        FGuid SelectedSkeletonGUID;
        int32 SelectedAxisCount = 2;
    };
}
