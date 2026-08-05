#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/PhysicsAsset/PhysicsAsset.h"
#include "GUID/GUID.h"
#include "PhysicsAssetFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CPhysicsAssetFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Physics Asset"; }
        FStringView GetDefaultAssetCreationName() override { return "NewPhysicsAsset"; }
        FString GetAssetDescription() const override { return "Ragdoll bodies and joint limits authored against a skeleton."; }
        CClass* GetAssetClass() const override { return CPhysicsAsset::StaticClass(); }
        FString GetCategory() const override { return "Physics"; }

        // Picks the skeleton up front; bodies are authored by bone name, so the editor is unusable without one.
        bool HasCreationDialogue() const override;
        bool DrawCreationDialogue(FStringView Path, bool& bShouldClose) override;

    private:

        FGuid SelectedSkeletonGUID;
    };
}
