#pragma once

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"

namespace Lumina
{
    // Inline editor for one FParticleParameter row in UserParameters; lays out
    // [Name] [Type] [typed value editor] on a line, Niagara-style.
    class FParticleParameterCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FParticleParameterCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FParticleParameter Value;
        char NameBuffer[128] = {};
    };
}
