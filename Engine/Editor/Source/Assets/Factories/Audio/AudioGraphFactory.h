#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Audio/AudioGraph.h"
#include "AudioGraphFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CAudioGraphFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Audio Graph"; }
        FStringView GetDefaultAssetCreationName() override { return "NewAudioGraph"; }
        FString GetAssetDescription() const override { return "A node based sound built from oscillators, wave players and effects."; }
        CClass* GetAssetClass() const override { return CAudioGraph::StaticClass(); }
        FString GetCategory() const override { return "Audio"; }
    };
}
