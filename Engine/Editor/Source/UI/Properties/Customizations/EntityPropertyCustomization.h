#pragma once

#include "UI/Properties/PropertyEditContexts.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // PROPERTY(Entity) uint32: a searchable picker over the active world context's entities.
    // Stored value is the entity's integral id; unset is entt::null's integral.
    class FEntityPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FEntityPropertyCustomization> MakeInstance()
        {
            return MakeShared<FEntityPropertyCustomization>();
        }

        // Cancels an in-flight eyedropper pick if this picker is torn down mid-pick
        // (e.g. the details panel rebuilds), so the viewport doesn't stay in pick mode.
        ~FEntityPropertyCustomization();

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        uint32                        CachedValue = 0;
        TSharedPtr<FEntityPickBroker> PickBroker;
    };
}
