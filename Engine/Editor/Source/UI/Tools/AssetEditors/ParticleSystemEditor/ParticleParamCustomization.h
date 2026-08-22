#pragma once

#include "Assets/AssetTypes/ParticleSystem/ParticleSystem.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"

namespace Lumina
{
    /** The particle system whose module properties are currently being drawn.
     *
     *  A property handle carries a container pointer and an FProperty, never an owner, so a customization
     *  editing a module input has no way to reach the system that declares the user parameters it could
     *  bind to -- and it could not walk up to one anyway, since module stacks are separate package objects
     *  rather than children of the asset. The editor tool announces it around the details panel instead.
     *
     *  A stack rather than a single pointer for the same reason the anim-graph picker uses one: nesting is
     *  possible and popping has to restore, not clear. Unset (a module property table drawn from anywhere
     *  else) simply means the bind menu lists nothing; the constant editor still works. */
    namespace ParticleParamContext
    {
        void PushSystem(CParticleSystem* System);
        void PopSystem();

        CParticleSystem* GetActiveSystem();

        struct FScope
        {
            explicit FScope(CParticleSystem* System) { PushSystem(System); }
            ~FScope()                                { PopSystem(); }
            FScope(const FScope&) = delete;
            FScope& operator=(const FScope&) = delete;
        };
    }

    /** Inline editor for one SParticleParam module input: the authored value, plus a menu that swaps it
     *  for a named user parameter the game drives at runtime.
     *
     *  Bound rows drop the value editor for the parameter name, the way Niagara does -- the constant is
     *  still there underneath, but showing an editable number that the running effect ignores is the
     *  single most confusing thing this widget could do. */
    class FParticleParamCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FParticleParamCustomization> MakeInstance();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        SParticleParam Value;
    };
}
