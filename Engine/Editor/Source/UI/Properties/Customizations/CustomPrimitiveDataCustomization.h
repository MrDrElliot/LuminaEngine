#pragma once

#include "ImGuiDrawUtils.h"
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

        // Edits buffer in Value, written via UpdatePropertyValue. Started/Finished across
        // two frames forms an undo transaction; multi-frame drags fold into the open one.
        bool bFinishPending = false;
    };
}
