#pragma once

#include "imgui.h"
#include "Containers/String.h"
#include "Core/Reflection/PropertyCustomization/PropertyCustomization.h"
#include "Input/InputAction.h"

namespace Lumina
{
    // Shared by the reflected handle below and the C# path, whose properties carry InputAction metadata.
    namespace InputActionPicker
    {
        const char* TypeLabel(EInputActionType Type);
        const char* TypeIcon(EInputActionType Type);

        // "W, S, Mouse X", or "Unbound".
        FString DescribeBindings(const SInputAction& Action);

        // True on the frame the user picked, writing the chosen name (empty for none) into Value.
        bool DrawCombo(const char* Id, FString& Value, ImGuiTextFilter& Filter);
    }

    // Chosen by type, so any component carrying a handle gets the picker without opting in per property.
    class FInputActionHandlePropertyCustomization : public IPropertyTypeCustomization
    {
    public:

        static TSharedPtr<FInputActionHandlePropertyCustomization> MakeInstance()
        {
            return MakeShared<FInputActionHandlePropertyCustomization>();
        }

        EPropertyChangeOp DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args) override;
        void UpdatePropertyValue(const TSharedPtr<FPropertyHandle>& Property) override;
        void HandleExternalUpdate(const TSharedPtr<FPropertyHandle>& Property) override;

    private:

        FInputActionHandle CachedValue;
        FInputActionHandle DisplayValue;
        ImGuiTextFilter    SearchFilter;

        // One-frame discrete edit: Started on the change frame, Finished the next, so undo sees a pair.
        bool bFinishPending = false;
    };
}
