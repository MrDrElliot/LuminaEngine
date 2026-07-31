#include "MaterialNode_Constants.h"

#include "Core/Object/Cast.h"
#include "Core/Math/Math.h"
#include "UI/Tools/NodeGraph/Material/MaterialOutput.h"
#include "UI/Tools/NodeGraph/Material/MaterialCompiler.h"

namespace Lumina
{
    void CMaterialExpression_Constant::DrawContextMenu()
    {
        const char* MenuItem = bDynamic ? "Make Constant" : "Make Parameter";
        if (ImGui::MenuItem(MenuItem))
        {
            bDynamic = !bDynamic;
            if (bDynamic)
            {
                ParameterName = FString(GetNodeDisplayName()) + "_Param";
            }
        }
    }

    void CMaterialExpression_Constant::BuildNode()
    {
        switch (ValueType)
        {
        case EMaterialInputType::Float:
            {
                CMaterialOutput* ValuePin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "X", ENodePinDirection::Output));
                ValuePin->SetShouldDrawEditor(true);
                ValuePin->SetHideDuringConnection(false);
                ValuePin->SetPinName("X");
                ValuePin->SetInputType(EMaterialInputType::Float);
            }
            break;
        case EMaterialInputType::Float2:
            {
                CMaterialOutput* ValuePin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "XY", ENodePinDirection::Output));
                ValuePin->SetShouldDrawEditor(true);
                ValuePin->SetHideDuringConnection(false);
                ValuePin->SetPinName("XY");
                ValuePin->SetComponentMask(EComponentMask::RG);
                ValuePin->SetInputType(EMaterialInputType::Float2);

                CMaterialOutput* R = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "X", ENodePinDirection::Output));
                R->SetPinColor(IM_COL32(255, 10, 10, 255));
                R->SetHideDuringConnection(false);
                R->SetPinName("X");
                R->SetComponentMask(EComponentMask::R);

                CMaterialOutput* G = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "Y", ENodePinDirection::Output));
                G->SetPinColor(IM_COL32(10, 255, 10, 255));
                G->SetHideDuringConnection(false);
                G->SetPinName("Y");
                G->SetComponentMask(EComponentMask::G);
            }
            break;
        case EMaterialInputType::Float3:
            {
                CMaterialOutput* ValuePin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "RGB", ENodePinDirection::Output));
                ValuePin->SetShouldDrawEditor(true);
                ValuePin->SetHideDuringConnection(false);
                ValuePin->SetPinName("RGB");
                ValuePin->SetComponentMask(EComponentMask::RGB);
                ValuePin->SetInputType(EMaterialInputType::Float3);

                CMaterialOutput* R = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "R", ENodePinDirection::Output));
                R->SetPinColor(IM_COL32(255, 10, 10, 255));
                R->SetHideDuringConnection(false);
                R->SetPinName("R");
                R->SetComponentMask(EComponentMask::R);

                CMaterialOutput* G = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "G", ENodePinDirection::Output));
                G->SetPinColor(IM_COL32(10, 255, 10, 255));
                G->SetHideDuringConnection(false);
                G->SetPinName("G");
                G->SetComponentMask(EComponentMask::G);

                CMaterialOutput* B = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "B", ENodePinDirection::Output));
                B->SetPinColor(IM_COL32(10, 10, 255, 255));
                B->SetHideDuringConnection(false);
                B->SetPinName("B");
                B->SetComponentMask(EComponentMask::B);
            }
            break;
        case EMaterialInputType::Float4:
            {
                CMaterialOutput* ValuePin = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "RGBA", ENodePinDirection::Output));
                ValuePin->SetShouldDrawEditor(true);
                ValuePin->SetHideDuringConnection(false);
                ValuePin->SetPinName("RGBA");
                ValuePin->SetComponentMask(EComponentMask::RGBA);
                ValuePin->SetInputType(EMaterialInputType::Float4);

                CMaterialOutput* R = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "R", ENodePinDirection::Output));
                R->SetPinColor(IM_COL32(255, 10, 10, 255));
                R->SetHideDuringConnection(false);
                R->SetPinName("R");
                R->SetComponentMask(EComponentMask::R);

                CMaterialOutput* G = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "G", ENodePinDirection::Output));
                G->SetPinColor(IM_COL32(10, 255, 10, 255));
                G->SetHideDuringConnection(false);
                G->SetPinName("G");
                G->SetComponentMask(EComponentMask::G);

                CMaterialOutput* B = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "B", ENodePinDirection::Output));
                B->SetPinColor(IM_COL32(10, 10, 255, 255));
                B->SetHideDuringConnection(false);
                B->SetPinName("B");
                B->SetComponentMask(EComponentMask::B);

                CMaterialOutput* A = Cast<CMaterialOutput>(CreatePin(CMaterialOutput::StaticClass(), "A", ENodePinDirection::Output));
                A->SetHideDuringConnection(false);
                A->SetPinName("A");
                A->SetComponentMask(EComponentMask::A);
            }
            break;
        }
    }

    void CMaterialExpression_ConstantFloat::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (bDynamic) Compiler.DefineFloatParameter(FullName, ParameterName, Value.r);
        else          Compiler.DefineConstantFloat(FullName, Value.r);
    }
    void CMaterialExpression_ConstantFloat::DrawNodeBody()
    {
        ImGui::SetNextItemWidth(126.0f);
        if (ImGui::DragFloat("##", Math::ValuePtr(Value), 0.01f))
        {
            NotifyValueEdited();
        }
    }

    void CMaterialExpression_ConstantFloat2::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (bDynamic)
        {
            Compiler.DefineFloat2Parameter(FullName, ParameterName, &Value.r);
        }
        else
        {
            Compiler.DefineConstantFloat2(FullName, &Value.r);
        }
    }
    void CMaterialExpression_ConstantFloat2::DrawNodeBody()
    {
        ImGui::SetNextItemWidth(126.0f);
        if (ImGui::DragFloat2("##", Math::ValuePtr(Value), 0.01f))
        {
            NotifyValueEdited();
        }
    }

    void CMaterialExpression_ConstantFloat3::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (bDynamic)
        {
            Compiler.DefineFloat3Parameter(FullName, ParameterName, &Value.r);
        }
        else
        {
            Compiler.DefineConstantFloat3(FullName, &Value.r);
        }
    }
    void CMaterialExpression_ConstantFloat3::DrawNodeBody()
    {
        ImGui::SetNextItemWidth(126.0f);
        if (ImGui::ColorPicker3("##", Math::ValuePtr(Value)))
        {
            NotifyValueEdited();
        }
    }

    void CMaterialExpression_ConstantFloat4::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        if (bDynamic)
        {
            Compiler.DefineFloat4Parameter(FullName, ParameterName, &Value.r);
        }
        else
        {
            Compiler.DefineConstantFloat4(FullName, &Value.r);
        }
    }
    void CMaterialExpression_ConstantFloat4::DrawNodeBody()
    {
        ImGui::SetNextItemWidth(126.0f);
        if (ImGui::ColorPicker4("##", Math::ValuePtr(Value)))
        {
            NotifyValueEdited();
        }
    }

    void CMaterialExpression_NumericConstant::BuildNode()
    {
        Super::BuildNode();
        Output->SetInputType(EMaterialInputType::Float);
        Output->SetComponentMask(EComponentMask::R);
    }

    FStringView CMaterialExpression_NumericConstant::GetNodeDisplayName() const
    {
        switch (Constant)
        {
        case EBuiltinConstant::Pi:          return "Pi";
        case EBuiltinConstant::TwoPi:       return "TwoPi";
        case EBuiltinConstant::HalfPi:      return "HalfPi";
        case EBuiltinConstant::E:           return "e";
        case EBuiltinConstant::Sqrt2:       return "Sqrt2";
        case EBuiltinConstant::GoldenRatio: return "GoldenRatio";
        case EBuiltinConstant::DegToRad:    return "DegToRad";
        case EBuiltinConstant::RadToDeg:    return "RadToDeg";
        case EBuiltinConstant::Zero:        return "Zero";
        case EBuiltinConstant::One:         return "One";
        default:                            return "Constant";
        }
    }

    void CMaterialExpression_NumericConstant::GenerateDefinition(FMaterialCompiler& Compiler)
    {
        float V = 0.0f;
        switch (Constant)
        {
        case EBuiltinConstant::Pi:          V = 3.14159265359f; break;
        case EBuiltinConstant::TwoPi:       V = 6.28318530718f; break;
        case EBuiltinConstant::HalfPi:      V = 1.57079632679f; break;
        case EBuiltinConstant::E:           V = 2.71828182846f; break;
        case EBuiltinConstant::Sqrt2:       V = 1.41421356237f; break;
        case EBuiltinConstant::GoldenRatio: V = 1.61803398875f; break;
        case EBuiltinConstant::DegToRad:    V = 0.01745329252f; break;
        case EBuiltinConstant::RadToDeg:    V = 57.29577951308f; break;
        case EBuiltinConstant::Zero:        V = 0.0f; break;
        case EBuiltinConstant::One:         V = 1.0f; break;
        }
        Compiler.NumericConstant(FullName, V);
    }
}
