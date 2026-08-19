#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Sequence/Sequence.h"
#include "SequenceFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CSequenceFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Sequence"; }
        FStringView GetDefaultAssetCreationName() override { return "NewSequence"; }
        FString GetAssetDescription() const override { return "A cutscene: entities bound to tracks that drive them over time."; }
        CClass* GetAssetClass() const override { return CSequence::StaticClass(); }

        // Grouped with Animation rather than a category of its own; Factory.h asks for a small shared set
        // of category strings, and a one-entry submenu is worse than a near neighbor.
        FString GetCategory() const override { return "Animation"; }
    };
}
