#include "CoreTypeCustomization.h"
#include "imgui.h"
#include "Core/Reflection/Type/LuminaTypes.h"

namespace Lumina
{
    EPropertyChangeOp FComponentRefPropertyCustomization::DrawProperty(const TSharedPtr<FPropertyHandle>& Property, const FPropertyDrawArgs& Args)
    {
        const FString Text = Property->Property->ToString(Property->GetValuePtr());
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", *Text);
        return EPropertyChangeOp::None;
    }
}
