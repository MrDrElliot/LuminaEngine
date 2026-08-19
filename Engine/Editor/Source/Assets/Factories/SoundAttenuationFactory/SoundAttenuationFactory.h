#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Audio/SoundAttenuation.h"
#include "SoundAttenuationFactory.generated.h"


namespace Lumina
{
    REFLECT()
    class CSoundAttenuationFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Sound Attenuation"; }
        FStringView GetDefaultAssetCreationName() override { return "NewSoundAttenuation"; }
        FString GetAssetDescription() const override { return "Shared distance falloff, cone and doppler settings for spatialized sounds."; }
        CClass* GetAssetClass() const override { return CSoundAttenuation::StaticClass(); }
        FString GetCategory() const override { return "Audio"; }
    };
}
