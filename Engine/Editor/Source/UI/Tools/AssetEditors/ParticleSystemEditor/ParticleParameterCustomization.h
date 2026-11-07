#pragma once

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"

namespace Lumina
{
    // Inline editor for one SParticleParameter row in UserParameters; lays out
    // [Name] [Type] [typed value editor] on a line, Niagara-style.
    class FParticleParameterCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FParticleParameterCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SParticleParameter Value;
        FString NameText;
    };
}
