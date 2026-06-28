#include "MaterialNodeExpression.h"

#include "Core/Object/Cast.h"
#include "Tools/UI/ImGui/ImGuiDesignIcons.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"

namespace Lumina
{
    void CMaterialExpression::BuildNode()
    {
        Output = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "", ENodePinDirection::Output));
        Output->SetShouldDrawEditor(false);
    }

    void CMaterialExpression::DrawNodeTitleBar()
    {
        if (bDynamic)
        {
            ImGui::Text(LE_ICON_MATERIAL_DESIGN " %s", GetNodeDisplayName().c_str());
            ImGuiX::TextTooltip("{}", "Edit this parameter in in the \"Material Properties\" window.");
        }
        else
        {
            ImGui::TextUnformatted(GetNodeDisplayName().c_str());
        }
    }

    void CMaterialExpression_Math::BuildNode()
    {
        Super::BuildNode();
    }
}
