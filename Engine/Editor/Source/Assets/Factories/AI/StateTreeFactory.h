#pragma once

#include "AI/StateTree/StateTree.h"
#include "Assets/Factories/Factory.h"
#include "StateTreeFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CStateTreeFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        // Also gives state tree tiles their outline, since this is where the editor first learns of the type.
        void PostCreateCDO() override;

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "State Tree"; }
        FStringView GetDefaultAssetCreationName() override { return "NewStateTree"; }
        FString GetAssetDescription() const override { return "Hierarchical states with tasks, conditions and transitions that drive an entity's behavior."; }
        CClass* GetAssetClass() const override { return CStateTree::StaticClass(); }
        FString GetCategory() const override { return "AI"; }
    };
}
