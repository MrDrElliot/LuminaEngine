#pragma once

#include "Assets/Factories/Factory.h"
#include "Assets/AssetTypes/Textures/RuntimeVirtualTexture.h"
#include "RuntimeVirtualTextureFactory.generated.h"

namespace Lumina
{
    REFLECT()
    class CRuntimeVirtualTextureFactory : public CFactory
    {
        GENERATED_BODY()
    public:

        CObject* CreateNew(const FName& Name, CPackage* Package) override;

        FString GetAssetName() const override { return "Runtime Virtual Texture"; }
        FStringView GetDefaultAssetCreationName() override { return "NewRuntimeVirtualTexture"; }
        FString GetAssetDescription() const override { return "A texture laid over the world that terrains and writers fill and materials sample by world position, such as ground color for grass or a trail history."; }
        CClass* GetAssetClass() const override { return CRuntimeVirtualTexture::StaticClass(); }
        FString GetCategory() const override { return "Texture"; }
    };
}
