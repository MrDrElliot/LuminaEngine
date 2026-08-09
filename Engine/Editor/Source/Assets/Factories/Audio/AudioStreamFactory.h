#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Audio/AudioStream.h"
#include "AudioStreamFactory.generated.h"


namespace Lumina
{
	REFLECT()
	class CAudioStreamFactory : public CFactory
	{
		GENERATED_BODY()
	public:

		CObject* CreateNew(const FName& Name, CPackage* Package) override;



		FString GetAssetName() const override { return "Audio Stream"; }
		FString GetCategory() const override { return "Audio"; }
		FStringView GetDefaultAssetCreationName() override { return "NewAudioStream"; }
		FString GetAssetDescription() const override { return "An audio clip."; }
		CClass* GetAssetClass() const override { return CAudioStream::StaticClass(); }

	};
}
