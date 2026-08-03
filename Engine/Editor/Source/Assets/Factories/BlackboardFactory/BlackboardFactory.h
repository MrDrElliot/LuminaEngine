#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Blackboard/Blackboard.h"
#include "BlackboardFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CBlackboardFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Blackboard"; }
        FStringView GetDefaultAssetCreationName() override { return "NewBlackboard"; }
        FString GetAssetDescription() const override { return "A blackboard: a named set of typed keys read/written at runtime by the animation graph and AI."; }
        CClass* GetAssetClass() const override { return CBlackboard::StaticClass(); }
        FString GetCategory() const override { return "Gameplay"; }
    };
}
