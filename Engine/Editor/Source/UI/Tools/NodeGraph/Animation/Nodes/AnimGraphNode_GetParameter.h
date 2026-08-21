#pragma once

#include "UI/Tools/NodeGraph/Animation/AnimGraphNode.h"
#include "AnimGraphNode_GetParameter.generated.h"

namespace Lumina
{
    REFLECT()
    class CAnimGraphNode_GetParameter : public CAnimGraphNode
    {
        GENERATED_BODY()
    public:

        FStringView GetNodeDisplayName() const override { return "Get Parameter"; }
        FStringView GetNodeTooltip() const override { return "Reads a named graph parameter as a scalar value."; }

        void BuildNode() override;
        void GenerateBytecode(FAnimationGraphCompiler& Compiler) override;

        /** Name of the parameter to read; must match the name set from Lua / the editor. */
        PROPERTY(Editable, Category = "Parameter", ParameterPicker)
        FName ParameterName;

        /** Value used when the parameter has not been set at runtime. */
        PROPERTY(Editable, Category = "Parameter")
        float DefaultValue = 0.0f;

        CAnimGraphPin* ValuePin = nullptr;
    };
}
