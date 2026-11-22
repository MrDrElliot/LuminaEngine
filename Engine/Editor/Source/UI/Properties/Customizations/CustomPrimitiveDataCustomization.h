#pragma once

#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Renderer/CustomPrimitiveData.h"


namespace Lumina
{
    class FCustomPrimDataPropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FCustomPrimDataPropertyCustomization> MakeInstance();
        
        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;

        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

        SCustomPrimitiveData Value;
        ImGuiTextFilter SearchFilter;

        // Edits buffer in Value and write via UpdatePropertyValue, with Started and Finished across two frames forming one undo transaction.
        bool bFinishPending = false;
    };
}
