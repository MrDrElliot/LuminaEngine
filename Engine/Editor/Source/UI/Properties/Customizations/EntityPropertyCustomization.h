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

        // Cancels an in-flight eyedropper pick on teardown so the viewport does not stay in pick mode.
        ~FEntityPropertyCustomization() override;
        FEntityPropertyCustomization() = default;
        FEntityPropertyCustomization(const FEntityPropertyCustomization&) = delete;
        FEntityPropertyCustomization& operator=(const FEntityPropertyCustomization&) = delete;
        FEntityPropertyCustomization(FEntityPropertyCustomization&&) = delete;
        FEntityPropertyCustomization& operator=(FEntityPropertyCustomization&&) = delete;

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        ECS::FEntity                  CachedValue = ECS::NullEntity;
        FPropertyEditSession          EditSession;
        TSharedPtr<FEntityPickBroker> PickBroker;
    };
}
