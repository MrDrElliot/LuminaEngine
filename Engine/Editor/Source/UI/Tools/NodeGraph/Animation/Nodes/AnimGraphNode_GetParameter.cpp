#include "AnimGraphNode_GetParameter.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    FString CAnimGraphNode_GetParameter::GetNodeTitleText() const
    {
        return ParameterName.IsNone() ? FString(GetNodeDisplayName()) : FString("Get ") + ParameterName.ToString();
    }

    void CAnimGraphNode_GetParameter::BuildNode()
    {
        ValuePin = CreateAnimPin("Value", ENodePinDirection::Output, EAnimPinType::Value);
    }

    void CAnimGraphNode_GetParameter::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        Compiler.ValidateParameterKey(ParameterName, this);

        const int32 ParameterIndex = Compiler.AddParameter(ParameterName, EAnimGraphParamType::Float, DefaultValue);
        const uint16 ValueReg = Compiler.EmitLoadParam((uint16)ParameterIndex);

        Compiler.SetPinRegister(ValuePin, ValueReg);
    }
}
