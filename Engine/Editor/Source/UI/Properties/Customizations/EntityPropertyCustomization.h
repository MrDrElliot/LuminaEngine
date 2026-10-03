#pragma once

#include "World/ECS/Registry.h"


#include "UI/Properties/PropertyEditContexts.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Memory/SmartPtr.h"
#include "Platform/GenericPlatform.h"

namespace Lumina
{
    // A searchable picker over the active world context's entities, for an ECS::FEntity property.
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

        ECS::FEntity                  CachedValue = ECS::NullEntity;
        FPropertyEditSession          EditSession;
        TSharedPtr<FEntityPickBroker> PickBroker;
    };
}
