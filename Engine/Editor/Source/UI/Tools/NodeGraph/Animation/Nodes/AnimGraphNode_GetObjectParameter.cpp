#include "AnimGraphNode_GetObjectParameter.h"
#include "UI/Tools/NodeGraph/Animation/AnimationGraphCompiler.h"

namespace Lumina
{
    FString CAnimGraphNode_GetObjectParameter::GetNodeTitleText() const
    {
        return ParameterName.IsNone() ? FString(GetNodeDisplayName()) : FString("Get ") + ParameterName.ToString();
    }

    void CAnimGraphNode_GetObjectParameter::BuildNode()
    {
        ObjectPin = CreateAnimPin("Object", ENodePinDirection::Output, EAnimPinType::Object);
    }

    void CAnimGraphNode_GetObjectParameter::GenerateBytecode(FAnimationGraphCompiler& Compiler)
    {
        Compiler.ValidateObjectParameterKey(ParameterName, ObjectType, this);

        if (ParameterName.IsNone())
        {
            EdNodeGraph::FError Warning;
            Warning.Name        = "Unbound Object Parameter";
            Warning.Description = "Get Object Parameter has no field assigned, so it will always evaluate to nothing.";
            Warning.Node        = this;
            Compiler.AddWarning(Warning);
        }

        const int32 ParamIndex = Compiler.AddObjectParameter(ParameterName, ObjectType);
        const uint16 ObjectReg = Compiler.EmitLoadObjectParam((uint16)ParamIndex);

        Compiler.SetPinRegister(ObjectPin, ObjectReg);
    }
}
