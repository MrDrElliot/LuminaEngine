#include "MaterialNode_Conditional.h"

#include "Core/Object/Cast.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

#include "MaterialNodePinHelpers.h"

namespace Lumina
{

    void CMaterialExpression_If::BuildNode()
    {
        Super::BuildNode();
        X         = MakeIn(this, "X");
        Y         = MakeIn(this, "Y");
        AGreater  = MakeIn(this, "X > Y");
        AEqual    = MakeIn(this, "X = Y");
        ALess     = MakeIn(this, "X < Y");
    }
    void CMaterialExpression_If::GenerateDefinition(FMaterialCompiler& C)
    {
        C.If(X, Y, AGreater, AEqual, ALess, Threshold);
    }

    void CMaterialExpression_Compare::BuildNode()
    {
        Super::BuildNode();
        A = MakeIn(this, "A");
        B = MakeIn(this, "B");
        Output->SetInputType(EMaterialInputType::Float);
    }
    void CMaterialExpression_Compare::GenerateDefinition(FMaterialCompiler& C)
    {
        FString OpStr;
        switch (Op)
        {
        case ECompareOp::Equal:        OpStr = "=="; break;
        case ECompareOp::NotEqual:     OpStr = "!="; break;
        case ECompareOp::Less:         OpStr = "<";  break;
        case ECompareOp::LessEqual:    OpStr = "<="; break;
        case ECompareOp::Greater:      OpStr = ">";  break;
        case ECompareOp::GreaterEqual: OpStr = ">="; break;
        }
        C.Compare(OpStr, A, B);
    }
}
