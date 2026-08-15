#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Animation/Montage/AnimationMontage.h"
#include "GUID/GUID.h"
#include "AnimationMontageFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CAnimationMontageFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Animation Montage"; }
        FStringView GetDefaultAssetCreationName() override { return "NewAnimationMontage"; }
        FString GetAssetDescription() const override { return "Clips stitched onto named slots, played from gameplay and layered into an animation graph."; }
        CClass* GetAssetClass() const override { return CAnimationMontage::StaticClass(); }
        FString GetCategory() const override { return "Animation"; }

        // Segments are authored per skeleton, so picking one up front keeps the clip picker filtered.
        bool HasCreationDialogue() const override;
        bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) override;

    private:

        FGuid SelectedSkeletonGUID;
        FGuid SelectedAnimationGUID;
    };
}
